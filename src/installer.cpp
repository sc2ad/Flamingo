#include "installer.hpp"
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <queue>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>

#include <fmt/ranges.h>

#include "fixups.hpp"
#include "hook-data.hpp"
#include "hook-installation-result.hpp"
#include "hook-metadata.hpp"
#include "page-allocator.hpp"
#include "target-data.hpp"
#include "util.hpp"

/// @brief This function is assigned to the orig of a hook when the hook in question has no fixups written.
extern "C" void no_fixups() {
  FLAMINGO_ABORT(
      "CALL TO ORIG ON FUNCTION WHERE NO ORIG IS PRESENT! THIS WOULD NORMALLY RESULT IN A REALLY ANNOYING JUMP TO 0!");
}
namespace {
using namespace flamingo;

/// @brief The set of all targets hooked. An ordered map so we can perform large-scale walks by doing binary search.
inline static std::map<TargetDescriptor, TargetData> targets;

struct HookNameMetadataHash {
  std::size_t operator()(HookNameMetadata const& k) const {
    return std::hash<std::string>()(k.name) ^ (std::hash<std::string>()(k.namespaze) << 1);
  }
};

/// @brief Topologically sorts the provided hooks by their priority constraints.
/// @param hooks The list of hooks to sort in place.
/// @return The sorted list of hooks that have cycles.
std::list<HookInfo> topological_sort_hooks_by_priority(std::list<HookInfo>& hooks) {
  // Ensure any "final" priority hooks are placed at the end while preserving relative order.
  std::vector<std::list<HookInfo>::iterator> finals;
  finals.reserve(std::distance(hooks.begin(), hooks.end()));
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    if (it->metadata.priority.is_final) {
      finals.push_back(it);
    }
  }
  for (auto& it : finals) {
    hooks.splice(hooks.end(), hooks, it);
  }

  {
    std::vector<std::string_view> hook_names;
    hook_names.reserve(hooks.size());
    for (auto const& hook : hooks) {
      hook_names.push_back(hook.metadata.name_info.name);
    }
    FLAMINGO_DEBUG("Initial hook order before topological sort: {}", fmt::join(hook_names, " -> "));
  }

  // now build topological graph
  // each hook has a requirement to be after certain other hooks in this graph
  // we also convert before requirements to after requirements for easier processing

  // build name to iterator map
  std::unordered_map<HookNameMetadata, std::list<HookInfo>::iterator, HookNameMetadataHash> name_to_iterator;
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    name_to_iterator[it->metadata.name_info] = it;
  }

  // A before B == B after A
  // HookInfo after strings
  // this graph represents all the hooks and their before dependencies
  // key must be before values
  std::unordered_map<HookNameMetadata, std::vector<HookNameMetadata>, HookNameMetadataHash> graph;
  graph.reserve(hooks.size());

  // finds all hooks that match the given filter
  auto findMatches = [&](HookNameFilter const& filter, HookNameMetadata self) {
    std::vector<HookNameMetadata> matches;
    matches.reserve(1);
    for (auto const& hook : hooks) {
      auto const& name = hook.metadata.name_info;
      // exclude self
      if (name == self) {
        continue;
      }
      if (name.matches(filter)) {
        matches.push_back(name);
      }
    }
    return matches;
  };

  //
  for (auto const& hook : hooks) {
    // build afters first
    // If this hook specifies that it must come after some other hooks (A),
    // then we must add edges A -> this_hook in the graph (so that A precedes this).
    for (auto const& afterFilter : hook.metadata.priority.afters) {
      auto matches = findMatches(afterFilter, hook.metadata.name_info);
      for (auto const& matched : matches) {
        graph[matched].push_back(hook.metadata.name_info);
      }
    }

    // now build from befores
    // If this hook requests to be before some matched hooks, add edges current -> matched
    for (auto const& beforeFilter : hook.metadata.priority.befores) {
      auto matches = findMatches(beforeFilter, hook.metadata.name_info);
      for (auto const& matched_before : matches) {
        graph[hook.metadata.name_info].push_back(matched_before);
      }
    }
  }

  // now topogologically sort in place
  // topological sort should use HookNameMetadata.matches(HookNameMetadata const& other) for matching
  // we cannot invalidate list iterators
  // for hooks with cycle, we keep them in their original order
  // we use Kahn's algorithm for topological sorting
  // https://en.wikipedia.org/wiki/Topological_sorting#Kahn's_algorithm
  std::list<HookInfo> sorted_hooks;
  std::unordered_map<HookNameMetadata, int, HookNameMetadataHash> in_degree;
  for (auto const& hook : hooks) {
    in_degree[hook.metadata.name_info] = 0;
  }

  // compute in-degrees
  for (auto const& [name, befores] : graph) {
    for (auto const& before : befores) {
      in_degree[before]++;
    }
  }

  // find all nodes with in_degree 0, preserving the original hooks list order
  std::queue<HookNameMetadata> zero_in_degree;
  for (auto const& hook : hooks) {
    auto it_deg = in_degree.find(hook.metadata.name_info);
    if (it_deg != in_degree.end() && it_deg->second == 0) {
      zero_in_degree.push(hook.metadata.name_info);
    }
  }

  while (!zero_in_degree.empty()) {
    auto current_name = zero_in_degree.front();
    zero_in_degree.pop();

    // find the iterator for this name
    auto it = name_to_iterator.find(current_name);
    if (it == name_to_iterator.end()) {
      // should not happen
      continue;
    }
    // move to sorted_hooks
    sorted_hooks.splice(sorted_hooks.end(), hooks, it->second);

    // decrease in_degree of afters
    auto const& befores = graph[current_name];
    for (auto const& before : befores) {
      in_degree[before]--;
      if (in_degree[before] == 0) {
        zero_in_degree.push(before);
      }
    }
  }

  {
    std::vector<std::string_view> hook_names;
    hook_names.reserve(sorted_hooks.size());
    for (auto const& hook : sorted_hooks) {
      hook_names.push_back(hook.metadata.name_info.name);
    }
    FLAMINGO_DEBUG("Flattened hook order after topological sort attempt: {}", fmt::join(hook_names, " -> "));
  }

  // now, any remaining hooks in `hooks` are part of cycles
  // append them in their original order and log a warning. Splicing invalidates
  // iterators, so consume from the front until empty to preserve original order.
  for (auto const& hook : hooks) {
    FLAMINGO_CRITICAL(
        "Detected cycle in hook priorities involving hook name: {}. Hooks involved in the cycle will remain in their "
        "original order.",
        hook.metadata.name_info);
    FLAMINGO_CRITICAL("After priorities for this hook were: {}", fmt::join(hook.metadata.priority.afters, ", "));
    FLAMINGO_CRITICAL("Before priorities for this hook were: {}", fmt::join(hook.metadata.priority.befores, ", "));
  }

  // replace original list with the sorted result using swap to avoid reallocation
  hooks.swap(sorted_hooks);

  {
    std::vector<std::string_view> hook_names;
    hook_names.reserve(hooks.size());
    for (auto const& hook : hooks) {
      hook_names.push_back(hook.metadata.name_info.name);
    }
    FLAMINGO_DEBUG("Final hook order after topological sort: {}", fmt::join(hook_names, " -> "));
  }

  // remaining hooks are cycles
  return sorted_hooks;
}

/// @brief Recompiles the hooks for the given target, updating their orig pointers as needed.
/// @param hooks The list of hooks installed on the target.
/// @param target_info The target descriptor for the target.
void recompile_hooks(std::list<HookInfo>& hooks, TargetDescriptor const& target_info) {
  // Find the target entry. Note that this assumes the handle is not invalidated.
  auto target_entry = targets.find(target_info);
  if (target_entry == targets.end()) {
    FLAMINGO_ABORT("Recompile hooks called on non-existent target!");
    return;
  }

  // Ensure the target jumps to the first hook.
  auto it = hooks.begin();
  if (std::next(it) == hooks.end()) {
    // Single hook: target -> hook, hook.orig -> fixups or no_fixups
    target_entry->second.fixups.target.WriteJump(it->hook_ptr);
    it->assign_orig(target_entry->second.metadata.metadata.need_orig
                        ? target_entry->second.fixups.fixup_inst_destination.addr.data()
                        : reinterpret_cast<void*>(&no_fixups));
    return;
  }

  // Multiple hooks: head, middles, tail
  // Head
  target_entry->second.fixups.target.WriteJump(it->hook_ptr);
  it->assign_orig(std::next(it)->hook_ptr);

  // Middles
  for (++it; std::next(it) != hooks.end(); ++it) {
    it->assign_orig(std::next(it)->hook_ptr);
  }

  // Tail
  // 'it' now refers to the last element
  it->assign_orig(target_entry->second.metadata.metadata.need_orig
                      ? target_entry->second.fixups.fixup_inst_destination.addr.data()
                      : reinterpret_cast<void*>(&no_fixups));
}

/// @brief Finds a suitable location to install the given hook on the target, respecting priority constraints.
/// @param hooks The list of hooks currently installed on the target.
/// @param hook_to_install The hook to install.
/// @return An iterator to the location where the hook was installed, or an error if installation is not possible.
Result<std::list<HookInfo>::iterator, installation::TargetBadPriorities> find_suitable_priority_location_for(
    std::list<HookInfo>& hooks, HookInfo&& hook_to_install) {
  using ResultT = Result<std::list<HookInfo>::iterator, installation::TargetBadPriorities>;
  // Install onto the target, respecting priorities.
  // Note that we may need to recompile some callbacks/fixups to change things
  // 1. Topological sort on our hooks that exist here by priority
  // - Find a suitable location where we can fit (note that we MAY need to recompile and move hooks around in order to
  // do this)
  // - First, walk all the hooks for a viable location, if we can find one. If we cannot, then we have to recompile
  // hooks.

  // Figure out 3 possible scenarios
  // if incoming is final, we must be at the end (unless the end is also final, then error)
  // if existing hooks have priority constraints that depend on us, we need to respect those (topologically sort)
  // otherwise, we can install at the first suitable location that fits

  // Also, if we have a final priority, we need to be the final hook, unless that hook is itself already marked as
  // final.
  if (hook_to_install.metadata.priority.is_final) {
    if (!hooks.empty() && hooks.back().metadata.priority.is_final) {
      // We cannot install here, we have a conflict
      return ResultT::Err(installation::TargetBadPriorities{
        hook_to_install.metadata, fmt::format("Cannot install a 'final' hook after another 'final' hook with name: {}",
                                              hooks.back().metadata.name_info) });
    }
    // Select the end to install at

    auto new_it = hooks.emplace(hooks.end(), std::move(hook_to_install));
    return ResultT::Ok(new_it);
  }

  // ok now we have a non-final hook
  // if existing hooks have priority constraints that depend on us, we need to respect those
  // therefore topological

  // If the incoming hook has any priority constraints, we may need a topological pass.
  bool requires_sort =
      !hook_to_install.metadata.priority.afters.empty() || !hook_to_install.metadata.priority.befores.empty();

  // If any existing hook has constraints that reference the incoming hook, we must sort.
  for (auto const& existing_hook : hooks) {
    for (auto const& after_filter : existing_hook.metadata.priority.afters) {
      if (after_filter.matches(hook_to_install.metadata.name_info)) {
        requires_sort = true;
        break;
      }
    }
    // if existing_hook requests to be before us, we cannot install before it
    for (auto const& before_filter : existing_hook.metadata.priority.befores) {
      if (before_filter.matches(hook_to_install.metadata.name_info)) {
        requires_sort = true;
        break;
      }
    }
    if (requires_sort) {
      break;
    }
  }

  // if we require a sort, do it then recompile
  if (requires_sort) {
    TargetDescriptor target{ hook_to_install.target };
    // Insert the new hook first so we can let topo sort place it correctly
    auto newIt = hooks.insert(hooks.begin(), std::move(hook_to_install));
    // if our hook has priority constraints, we need to topologically sort and find a suitable location
    auto cycles = topological_sort_hooks_by_priority(hooks);

    if (!cycles.empty()) {
      // We have cycles involving our new hook
      // Remove our new hook
      std::vector<std::string> cycle_strings;
      for (auto const& hook : cycles) {
        cycle_strings.push_back(hook.metadata.name_info.name);
      }

      return ResultT::Err(installation::TargetBadPriorities{
        hook_to_install.metadata, fmt::format("Cannot install hook due to cycles in priorities involving hook name: {}",
                                              fmt::join(cycle_strings, ",")) });
    }

    // now recompile all hooks to ensure orig pointers are correct
    recompile_hooks(hooks, target);

    return ResultT::Ok(newIt);
  }

  // fast track If the incoming hook has no explicit constraints, insert at the front
  // so newer installs are called before earlier ones (preserve expected install semantics).
  if (hook_to_install.metadata.priority.afters.empty() && hook_to_install.metadata.priority.befores.empty()) {
    auto newIt = hooks.emplace(hooks.begin(), std::move(hook_to_install));
    return ResultT::Ok(newIt);
  }

  // if no priority constraints affect us, we can install at the first suitable location that fits
  // Linear search for a suitable location: insert before the first existing hook that we should come after.
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    bool can_install_before = true;
    for (auto const& after_filter : hook_to_install.metadata.priority.afters) {
      if (after_filter.matches(it->metadata.name_info)) {
        can_install_before = false;
        break;
      }
    }
    if (!can_install_before) {
      continue;
    }

    auto new_it = hooks.emplace(it, std::move(hook_to_install));
    return ResultT::Ok(new_it);
  }

  // If we could not find any suitable location, install at the start
  auto new_it = hooks.emplace(hooks.begin(), std::move(hook_to_install));
  return ResultT::Ok(new_it);
}

Result<std::monostate, installation::TargetMismatch> validate_install_metadata(TargetMetadata& existing,
                                                                               HookMetadata const& incoming) {
  using ResultT = Result<std::monostate, installation::TargetMismatch>;
  // 1. Take the min of num_insts/verify they are equivalent
  existing.method_num_insts = std::min(existing.method_num_insts, incoming.method_num_insts);
  // 2. Validate calling convention matches
  if (existing.convention != incoming.convention) {
    return ResultT::ErrAt<installation::MismatchTargetConv>(incoming, existing.convention);
  }
  // 3. Validate midpoint matches
  if (existing.metadata.is_midpoint != incoming.installation_metadata.is_midpoint) {
    return ResultT::ErrAt<installation::MismatchMidpoint>(incoming, existing.metadata.is_midpoint);
  }
  // 4. Ensure parameter_info and return_info are matching (ifdef guarded)
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
  if (existing.return_info != incoming.return_info) {
    return ResultT::ErrAt<installation::MismatchReturn>(incoming, existing.return_info);
  }
  if (existing.parameter_info.size() != incoming.parameter_info.size()) {
    return ResultT::ErrAt<installation::MismatchParamCount>(incoming, existing.parameter_info.size());
  }
  for (size_t i = 0; i < existing.parameter_info.size(); i++) {
    if (existing.parameter_info[i] != incoming.parameter_info[i]) {
      return ResultT::ErrAt<installation::MismatchParam>(incoming, i, existing.parameter_info[i]);
    }
  }
#endif
  return ResultT::Ok();
}

}  // namespace

namespace flamingo {
std::optional<TargetData const> TargetDataFor(TargetDescriptor target) {
  auto it = targets.find(target);
  if (it == targets.end()) {
    return std::nullopt;
  }
  return it->second;
}

installation::Result Install(HookInfo&& hook) {
  // Null targets to install to are prohibited, but null hook functions are allowed (and will most likely cause
  // horrible crashes when called)
  if (hook.target == nullptr) {
    return installation::Result::Err(installation::TargetIsNull{ hook.metadata.name_info });
  }
  TargetDescriptor target_info{ hook.target };
  auto hooked_target = targets.find(target_info);
  if (hooked_target == targets.end()) {
    // To make the first hook, we need to create the TargetData
    // For leapfrog hooks, we need to do something special anyways.
    // TODO: Support leapfrog hooks (where the installation space is fewer than 4U)
    // If we have an orig, we need to have an instruction to jump back to
    auto const method_size = Fixups::kNormalFixupInstCount + (hook.orig_ptr != nullptr ? 1 : 0);
    if (hook.metadata.method_num_insts < method_size) {
      return installation::Result::ErrAt<installation::TargetTooSmall>(hook.metadata, method_size);
    }
    // The initial protection of the page that holds the target
    auto target_initial_protection = PageProtectionType::kExecute | PageProtectionType::kRead;
    if (hook.metadata.installation_metadata.write_prot) {
      target_initial_protection |= PageProtectionType::kWrite;
    }
    auto target_pointer = PointerWrapper<uint32_t>(
        std::span<uint32_t>(reinterpret_cast<uint32_t*>(hook.target),
                            reinterpret_cast<uint32_t*>(hook.target) + hook.metadata.method_num_insts),
        target_initial_protection);
    auto result = targets.emplace(
        target_info, TargetData{ .metadata =
                                     TargetMetadata{
                                       .target = target_pointer,
                                       .convention = hook.metadata.convention,
                                       .metadata = hook.metadata.installation_metadata,
                                       .method_num_insts = hook.metadata.method_num_insts,
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
                                       .parameter_info = hook.metadata.parameter_info,
                                       .return_info = hook.metadata.return_info,
#endif
                                     },
                                 .fixups = Fixups{
                                   // Our fixup target is a subspan the same size as our install size
                                   .target = { target_pointer.Subspan(Fixups::kNormalFixupInstCount) },
                                   .fixup_inst_destination =
                                       Allocate(kHookAlignment,
                                                std::min(Page::PageSize, hook.metadata.method_num_insts *
                                                                             sizeof(uint32_t) * kNumFixupsPerInst),
                                                PageProtectionType::kExecute | PageProtectionType::kRead),
                                 } });
    auto& target_data = result.first->second;
    hook.assign_orig(reinterpret_cast<void*>(&no_fixups));
    // Always copy over our original instructions to our .fixups instance
    target_data.fixups.CopyOriginalInsts();
    // If we want to make an orig, we fill it out now
    if (hook.metadata.installation_metadata.need_orig) {
      target_data.fixups.PerformFixupsAndCallback();
      hook.assign_orig(target_data.fixups.fixup_inst_destination.addr.data());
    }
    // Add the hook itself to the set of hooks we have, taking ownership
    auto const hook_data_result = target_data.hooks.emplace(target_data.hooks.end(), std::move(hook));
    // Now actually INSTALL the hook at target to point to the first hook in target_data.hooks
    target_data.fixups.target.WriteJump(hook_data_result->hook_ptr);
    return installation::Result::Ok(flamingo::installation::Ok{ HookHandle{ .hook_location = hook_data_result } });
  }
  auto installation_checks = validate_install_metadata(hooked_target->second.metadata, hook.metadata);
  if (!installation_checks.has_value()) {
    return installation::Result::ErrAt<installation::TargetMismatch>(installation_checks.error());
  }

  auto location_or_err = find_suitable_priority_location_for(hooked_target->second.hooks, std::move(hook));
  if (!location_or_err.has_value()) {
    return installation::Result::ErrAt<installation::TargetBadPriorities>(location_or_err.error());
  }
  auto const hook_data_result = location_or_err.value();

  // TODO: Recompile fixups/origs for all hooks on this target or not?
  // recompile_hooks(hooked_target->second.hooks, target_info);

  // - This is done by looking to the left and right of our target iterator to insert at:
  // -- If left does not exist: Rewrite the jump from the target to us; else rewrite the left's orig final jump to us
  if (hook_data_result == hooked_target->second.hooks.begin()) {
    hooked_target->second.fixups.target.WriteJump(hook_data_result->hook_ptr);
  } else {
    std::prev(hook_data_result)->assign_orig(hook_data_result->hook_ptr);
  }
  // -- If right does not exist: OUR orig calls the overall fixups; else jump to their hook_ptr
  if (std::next(hook_data_result) == hooked_target->second.hooks.end()) {
    hook_data_result->assign_orig(hooked_target->second.fixups.fixup_inst_destination.addr.data());
  } else {
    hook_data_result->assign_orig(std::next(hook_data_result)->hook_ptr);
  }
  // TODO: Make assign_orig calls respect if we actually want an orig or not and add tests for this
  return installation::Result::Ok(flamingo::installation::Ok{ HookHandle{ .hook_location = hook_data_result } });
}

Result<bool, installation::Error> Reinstall(TargetDescriptor target) {
  using RetType = Result<bool, installation::Error>;
  auto itr = targets.find(target);
  if (itr == targets.end()) {
    return RetType::Ok(false);
  }
  // Reinstall the orig by calling PerformFixupsAndCallback() again (as needed)
  itr->second.fixups.CopyOriginalInsts();
  if (itr->second.metadata.metadata.need_orig) {
    itr->second.fixups.PerformFixupsAndCallback();
  }
  // Perform the write of the jump to the first hook
  itr->second.fixups.target.WriteJump(itr->second.hooks.begin()->hook_ptr);
  // Note that we do NOT reconstruct all of the inner hook pointers between each hook.
  // This is done as a partial optimization, but at some point we should revisit this (and adjust the docstring comment
  // to match)
  // TODO: Above
  return RetType::Ok(true);
}

Result<bool, bool> Uninstall(HookHandle handle) {
  using RetType = Result<bool, bool>;
  // Find the target entry. Note that this assumes the handle is not invalidated.
  auto target_entry = targets.find(TargetDescriptor(handle.hook_location->target));
  if (target_entry == targets.end()) {
    return RetType::Err(false);
  }
  // 1. If it is the only hook, destroys the fixups, uninstalls the hook by replacing the original instructions. Note
  // that this also destroys leapfrog hooks.
  if (target_entry->second.hooks.size() == 1) {
    target_entry->second.fixups.Uninstall();
    // At this point the original memory at our target is restored, we are safe to clear out the target entry here and
    // return
    // TODO: Invalidate leapfrog entries
    // TODO: Cleanup whatever dangling pointers we would have here (the fixup pointer being one of them)
    targets.erase(target_entry);
    return RetType::Ok(false);
  }
  // 2. If this is the first hook in a set of many, rewrites the target to jump to the hook past this one. Note that
  // this MAY also break leapfrog hooks, if this hook was installed as a branch but the next hook needs to be larger.
  if (handle.hook_location == target_entry->second.hooks.begin()) {
    target_entry->second.fixups.target.WriteJump(std::next(handle.hook_location)->hook_ptr);
  }
  // 3. If this is the last hook, makes the previous hook's orig point to the fixups directly, or to the no_fixups
  // function.
  else if (std::next(handle.hook_location) == target_entry->second.hooks.end()) {
    std::prev(handle.hook_location)
        ->assign_orig(target_entry->second.metadata.metadata.need_orig
                          ? target_entry->second.fixups.fixup_inst_destination.addr.data()
                          : reinterpret_cast<void*>(&no_fixups));
  }
  // 4. If this is a hook in the middle, the hook before us's orig will point to the next hook's hook function.
  else {
    std::prev(handle.hook_location)->assign_orig(std::next(handle.hook_location)->hook_ptr);
  }
  // After all that is done, the iterator is removed from the list of all hooks, and if empty, the entry from the
  // targets map is destroyed. Note that this invalidates all other held HookHandles to the SAME entry. Other entries
  // will not be invalidated.
  target_entry->second.hooks.erase(handle.hook_location);
  return RetType::Ok(true);
}

std::span<uint32_t> OriginalInstsFor(TargetDescriptor target) {
  auto itr = targets.find(target);
  if (itr != targets.end()) {
    return itr->second.fixups.original_instructions;
  }
  return {};
}

Result<TargetMetadata, std::monostate> MetadataFor(TargetDescriptor target) {
  auto itr = targets.find(target);
  if (itr != targets.end()) {
    return Result<TargetMetadata, std::monostate>::Ok(itr->second.metadata);
  }
  return Result<TargetMetadata, std::monostate>::Err();
}

Result<std::span<uint32_t const>, std::monostate> FixupPointerFor(TargetDescriptor target) {
  auto itr = targets.find(target);
  if (itr != targets.end()) {
    return Result<std::span<uint32_t const>, std::monostate>::Ok(itr->second.fixups.fixup_inst_destination.addr);
  }
  return Result<std::span<uint32_t const>, std::monostate>::Err();
}

}  // namespace flamingo