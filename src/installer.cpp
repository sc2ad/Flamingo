#include "installer.hpp"
#include <iterator>
#include <map>
#include "util.hpp"

/// @brief This function is assigned to the orig of a hook when the hook in question has no fixups written.
extern "C" void no_fixups() {
  FLAMINGO_ABORT(
      "CALL TO ORIG ON FUNCTION WHERE NO ORIG IS PRESENT! THIS WOULD NORMALLY RESULT IN A REALLY ANNOYING JUMP TO 0!");
}
namespace {
/// @brief The set of all targets hooked. An ordered map so we can perform large-scale walks by doing binary search.
inline static std::map<flamingo::TargetDescriptor, flamingo::TargetData> targets;
}  // namespace

namespace flamingo {
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
    constexpr static auto kInstallSize = Fixups::kNormalFixupInstCount;
    if (hook.metadata.method_num_insts >= Fixups::kNormalFixupInstCount) {
      return installation::Result::Err(installation::TargetTooSmall{ hook.metadata, Fixups::kNormalFixupInstCount });
    }
    auto target_pointer = PointerWrapper<uint32_t>(
        std::span<uint32_t>(reinterpret_cast<uint32_t*>(hook.target),
                            reinterpret_cast<uint32_t*>(hook.target) + hook.metadata.method_num_insts),
        PageProtectionType::kExecute | PageProtectionType::kRead);
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
                                 .orig = Fixups{
                                   // Our fixup target is a subspan the same size as our install size
                                   .target = { target_pointer.Subspan(kInstallSize) },
                                   .fixup_inst_destination =
                                       Allocate(kHookAlignment,
                                                std::min(Page::PageSize, hook.metadata.method_num_insts *
                                                                             sizeof(uint32_t) * kNumFixupsPerInst),
                                                PageProtectionType::kExecute | PageProtectionType::kRead),
                                 } });
    auto& target_data = result.first->second;
    hook.assign_orig(reinterpret_cast<void*>(&no_fixups));
    // If we want to make an orig, we fill it out now
    if (hook.metadata.installation_metadata.need_orig) {
      target_data.orig.PerformFixupsAndCallback();
      hook.assign_orig(target_data.orig.fixup_inst_destination.addr.data());
    }
    // Add the hook itself to the set of hooks we have, taking ownership
    auto const hook_data_result = target_data.hooks.emplace(target_data.hooks.end(), std::move(hook));
    // Now actually INSTALL the hook at target to point to the first hook in target_data.hooks
    target_data.orig.target.WriteJump(hook_data_result->hook_ptr);
    return installation::Result::Ok(flamingo::installation::Ok{ HookHandle{ .hook_location = hook_data_result } });
  } else {
    // Install onto the target, respecting priorities.
    // Note that we may need to recompile some callbacks/fixups to change things
    return installation::Result{ std::in_place_type_t<installation::Error>{}, {} };
  }
}

Result<bool, installation::Error> Reinstall(TargetDescriptor target) {
  using RetType = Result<bool, installation::Error>;
  auto itr = targets.find(target);
  if (itr == targets.end()) {
    return RetType::Ok(false);
  }
  // Reinstall the orig by calling PerformFixupsAndCallback() again (as needed)
  if (itr->second.metadata.metadata.need_orig) {
    itr->second.orig.PerformFixupsAndCallback();
  }
  // Perform the write of the jump to the first hook
  itr->second.orig.target.WriteJump(itr->second.hooks.begin()->hook_ptr);
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
    target_entry->second.orig.Uninstall();
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
    target_entry->second.orig.target.WriteJump(std::next(handle.hook_location)->hook_ptr);
  }
  // 3. If this is the last hook, makes the previous hook's orig point to the fixups directly, or to the no_fixups
  // function.
  else if (std::next(handle.hook_location) == target_entry->second.hooks.end()) {
    std::prev(handle.hook_location)
        ->assign_orig(target_entry->second.metadata.metadata.need_orig
                          ? target_entry->second.orig.fixup_inst_destination.addr.data()
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
}  // namespace flamingo