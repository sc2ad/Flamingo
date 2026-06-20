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

// Rebuild the per-target priority graph from the current hooks list.
void rebuild_priority_graph(TargetData& target_data) {
  target_data.priority_graph.clear();

  // First, create nodes for every hook with iterator
  for (auto it = target_data.hooks.begin(); it != target_data.hooks.end(); ++it) {
    target_data.priority_graph[it->metadata.name_info] = TargetData::GraphNode{ .hook_it = it, .afters = {} };
  }

  // Helper to find matches for a filter among current hooks
  auto findMatches = [&](HookNameFilter const& filter) {
    std::vector<HookNameMetadata> matches;
    for (auto const& hook : target_data.hooks) {
      if (filter.matches(hook.metadata.name_info)) {
        matches.push_back(hook.metadata.name_info);
      }
    }
    return matches;
  };

  // Build adjacency (afters) using same semantics as previous implementation
  for (auto const& hook : target_data.hooks) {
    // afters: for each afterFilter, matched -> this_hook
    for (auto const& afterFilter : hook.metadata.priority.afters) {
      auto matches = findMatches(afterFilter);
      for (auto const& matched : matches) {
        target_data.priority_graph[matched].afters.push_back(hook.metadata.name_info);
      }
    }
    // befores: current -> matched_before
    for (auto const& beforeFilter : hook.metadata.priority.befores) {
      auto matches = findMatches(beforeFilter);
      for (auto const& matched_before : matches) {
        target_data.priority_graph[hook.metadata.name_info].afters.push_back(matched_before);
      }
      }
    }
  }

// Topologically sort hooks for a single target using its persisted graph.
static Result<std::list<HookInfo>, installation::TargetBadPriorities> topological_sort_target(TargetData& target_data) {
  using ResultT = Result<std::list<HookInfo>, installation::TargetBadPriorities>;

  std::list<HookInfo> sorted_hooks;

  // build in-degree map
  std::unordered_map<HookNameMetadata, int> in_degree;
  for (auto const& pair : target_data.priority_graph) {
    in_degree[pair.first] = 0;
  }

  // calculate in-degrees
  // Note that we only consider nodes in the graph for topological sorting,
  // which means that hooks without priority constraints will not be affected by this process and will remain in their original order at the end of the sorted list.
  // This is intentional, as it allows us to preserve the original order of hooks that don't have any priority constraints, while still ensuring that hooks with constraints are ordered correctly.
  for (auto const& [name, node] : target_data.priority_graph) {
    for (auto const& after : node.afters) {
      in_degree[after]++;
    }
  }

  // queue of zero in-degree nodes, preserve original hooks list order
  std::queue<HookNameMetadata> zero_in_degree;
  for (auto const& hook : target_data.hooks) {
    auto it_deg = in_degree.find(hook.metadata.name_info);
    if (it_deg != in_degree.end() && it_deg->second == 0) {
      zero_in_degree.push(hook.metadata.name_info);
    }
  }

  while (!zero_in_degree.empty()) {
    auto current_name = zero_in_degree.front();
    zero_in_degree.pop();

    auto it_node = target_data.priority_graph.find(current_name);
    if (it_node == target_data.priority_graph.end()) {
      continue;  // shouldn't happen
    }

    // move node's hook iterator into sorted_hooks
    sorted_hooks.splice(sorted_hooks.end(), target_data.hooks, it_node->second.hook_it);

    for (auto const& after : it_node->second.afters) {
      in_degree[after]--;
      if (in_degree[after] == 0) {
        zero_in_degree.push(after);
      }
    }
  }

  // if any hooks remain, they are part of cycles
  if (!target_data.hooks.empty()) {
    std::vector<HookInfo> cycles;
    for (auto const& h : target_data.hooks) cycles.push_back(h);

    if (!target_data.hooks.empty()) {
      for (auto const& hook : target_data.hooks) {
      FLAMINGO_DEBUG(
            "Detected cycle in hook priorities involving hook name: {}. Hooks involved in the cycle will remain in "
            "their "
          "original order.",
          hook.metadata.name_info);
      FLAMINGO_DEBUG("After priorities for this hook were: {}", fmt::join(hook.metadata.priority.afters, ", "));
      FLAMINGO_DEBUG("Before priorities for this hook were: {}", fmt::join(hook.metadata.priority.befores, ", "));
      }
    }

    return ResultT::Err(installation::TargetBadPriorities{
      target_data.hooks.front().metadata, fmt::format("Detected cycle in hook priorities involving hooks. Hooks "
                                          "involved in the cycle will remain in their original order.") });
  }

  // swap sorted back into target_data.hooks
  target_data.hooks.swap(sorted_hooks);

  return ResultT::Ok(sorted_hooks);
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

  // TODO: Do we need to copy Reinstall logic here?

  // Reinstall the orig by calling PerformFixupsAndCallback() again (as needed)
  // Perform the write of the jump to the first hook
  // Head
  auto it = hooks.begin();
  target_entry->second.fixups.target.WriteJump(it->hook_ptr);

  while (std::next(it) != hooks.end()) {
    it->assign_orig(std::next(it)->hook_ptr);
    ++it;
  }
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
    // we don't validate here since it's done in the Install function
    // Select the end to install at

    hooks.emplace_back(std::move(hook_to_install));
    return ResultT::Ok(--hooks.end());
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
    // copy hooks
    auto old_hooks = hooks;

    TargetDescriptor target{ hook_to_install.target };
    auto metadata = hook_to_install.metadata;
    // Insert the new hook first so we can let topo sort place it correctly
    auto newIt = hooks.insert(hooks.begin(), std::move(hook_to_install));
    // if our hook has priority constraints, we need to topologically sort and find a suitable location
    auto trg_it = targets.find(target);
    if (trg_it == targets.end()) {
      // should not happen
      hooks.swap(old_hooks);
      return ResultT::Err(installation::TargetBadPriorities{ metadata, "Target missing during priority sorting" });
    }
    auto& target_data = trg_it->second;
    rebuild_priority_graph(target_data);
    auto cyclesResult = topological_sort_target(target_data);
    if (!cyclesResult.has_value()) {
      // revert hooks (we need to keep original order)
      hooks.swap(old_hooks);
      return ResultT::Err(cyclesResult.error());
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

Result<std::monostate, installation::TargetBadPriorities> validate_priority_constraints_for_new_hook(
    std::list<HookInfo> const& existing_hooks, HookMetadata const& incoming) {
  using ResultT = Result<std::monostate, installation::TargetBadPriorities>;
  // Validate that the new hook's priorities do not conflict with existing hooks
  // For each existing hook, we need to ensure that if the new hook wants to be before it, it is actually before it, and
  // if it wants to be after it, it is actually after it. We also need to ensure that if the new hook has a final
  // priority, it is the last hook.

  // Check final priority constraints first
  if (incoming.priority.is_final) {
    // we can just check the end because final hooks must be at the end
    if (!existing_hooks.empty() && existing_hooks.back().metadata.priority.is_final) {
      return ResultT::Err(installation::TargetBadPriorities{
        incoming, fmt::format("Cannot install a 'final' hook after another 'final' hook with name: {}",
                              existing_hooks.back().metadata.name_info) });
    }
  }

  // Prevent a hook from declaring priorities that reference itself (self-priority).
  for (auto const& afterFilter : incoming.priority.afters) {
    if (afterFilter.matches(incoming.name_info)) {
      return ResultT::Err(installation::TargetBadPriorities{
        incoming,
        fmt::format("Cannot install hook because it requests to be after itself: {}", incoming.name_info)});
    }
  }
  for (auto const& beforeFilter : incoming.priority.befores) {
    if (beforeFilter.matches(incoming.name_info)) {
      return ResultT::Err(installation::TargetBadPriorities{
        incoming,
        fmt::format("Cannot install hook because it requests to be before itself: {}", incoming.name_info)});
    }
  }

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

                                 },
                                 .priority_graph = {},
                               });
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
    // Build the initial priority graph for this target now that we have a hook
    rebuild_priority_graph(target_data);
    // Now actually INSTALL the hook at target to point to the first hook in target_data.hooks
    target_data.fixups.target.WriteJump(hook_data_result->hook_ptr);
    return installation::Result::Ok(flamingo::installation::Ok{ HookHandle{ .hook_location = hook_data_result } });
  }
  auto installation_checks = validate_install_metadata(hooked_target->second.metadata, hook.metadata);
  if (!installation_checks.has_value()) {
    return installation::Result::ErrAt<installation::TargetMismatch>(installation_checks.error());
  }

  auto priority_checks = validate_priority_constraints_for_new_hook(hooked_target->second.hooks, hook.metadata);
  if (!priority_checks.has_value()) {
    return installation::Result::ErrAt<installation::TargetBadPriorities>(priority_checks.error());
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
  // Rebuild the priority graph to reflect the removed hook
  rebuild_priority_graph(target_entry->second);
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

std::vector<HookInfo> Hooks(std::optional<HookNameFilter> const& filter,
                            std::optional<TargetDescriptor> const& targetFilter) {
  std::vector<HookInfo> out;
  out.reserve(16);
  for (auto const& target_pair : targets) {
    // If a target filter is provided, skip other targets
    if (targetFilter.has_value()) {
      if (target_pair.first.target != targetFilter->target) {
        continue;
      }
    }
    for (auto const& hook : target_pair.second.hooks) {
      if (filter.has_value()) {
        if (!filter->matches(hook.metadata.name_info)) continue;
      }
      out.push_back(hook);
    }
  }
  return out;
}

}  // namespace flamingo