

#include <algorithm>
#include <utility>
#include "fixups.hpp"
#include "hook-data.hpp"
#include "hook-installation-result.hpp"
#include "page-allocator.hpp"
#include "target-data.hpp"
#include "util.hpp"

namespace flamingo {

constexpr static auto kHookAlignment = 16U;
constexpr static auto kNumFixupsPerInst = 2U;

inline static std::unordered_map<TargetDescriptor, TargetData> targets;

/// @brief To install a hook, we require a constructed HookInfo. We want to hold exclusive ownership, so we require an
/// rvalue (we may also forward params?). Because a HookInfo is just data, we go find our TargetInfo that matches our
/// target. Then, we attempt to install that HookInfo onto the TargetData, mutating the TargetData (but not invalidating
/// other HookInfo references within the list). We update the shared information within the HookInfo and perform the
/// install as necessary. Priorities use named IDs for cleaer ordering (before x, after y). This may require a full
/// reassmebly of the list!
inline installation::Result Install(HookInfo&& hook) {
  // The set of all targets of hooks that are installed to
  TargetDescriptor target_info{ hook.target };
  auto hooked_target = targets.find(target_info);
  if (hooked_target == targets.end()) {
    // To make the first hook, we need to create the TargetData
    // For leapfrog hooks, we need to do something special anyways.
    // TODO: Support leapfrog hooks (where the installation space is fewer than 4U)
    constexpr static auto kInstallSize = Fixups::kNormalFixupInstCount;
    FLAMINGO_ASSERT(hook.metadata.method_num_insts >= Fixups::kNormalFixupInstCount);
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
    // If we want to make an orig, we fill it out now
    if (hook.metadata.installation_metadata.need_orig) {
      target_data.orig.PerformFixupsAndCallback();
    }
    // Add the hook itself to the set of hooks we have, taking ownership
    auto const hook_data_result = target_data.hooks.emplace(target_data.hooks.end(), std::move(hook));
    // Now actually INSTALL the hook at target to point to the first hook in target_data.hooks
    target_data.orig.target.WriteJump(hook_data_result->hook_ptr);
    return installation::Result{ std::in_place_type_t<installation::Ok>{},
                                 flamingo::installation::Ok{ HookHandle{ .hook_location = hook_data_result } } };
  } else {
    // Install onto the target, respecting priorities.
    // Note that we may need to recompile some callbacks/fixups to change things
    return installation::Result{ std::in_place_type_t<installation::Error>{}, {} };
  }
}

/// @brief Called on a target to reinstall all targets present at that location.
/// A reinstall is done by re-performing orig fixups at the target, and rewriting a jump to the first hook.
/// All other hooks remain unchanged.
/// This function returns Ok(true) if all hooks were reinstalled correctly, Ok(false) if there were no hooks to
/// reinstall, and Error(...) otherwise.
inline Result<bool, installation::Error> Reinstall(TargetDescriptor target) {
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
}  // namespace flamingo