#pragma once
#include <cstddef>
#include <cstdint>

#include "calling-convention.hpp"
#include "hook-metadata.hpp"

namespace flamingo {

/// @brief Represents a hook that a user of this library will use.
/// On install, we collect this information into a single TargetInfo structure, which contains a collection of multiple
/// Hook references. We map target --> TargetInfo and every time we have a new hook installed there, we move orig
/// pointers around accordingly. To do priorities, we have to track that state within a given Hook We don't necessarily
/// need O(1) hook installation, but we could. A TargetInfo may basically just be a Hook, except with the added list of
/// Hooks, which we use to validate.
struct HookInfo {
  // TODO: friend struct this to something?
  friend struct TargetData;
  template <class R, class... TArgs>
  using HookFuncType = R (*)(TArgs...);

  /// @brief The default number of instructions to install a hook with
  constexpr static uint16_t kDefaultNumInsts = 10U;

  template <class R, class... TArgs>
  HookInfo(HookFuncType<R, TArgs...> hook_func, void* target, HookFuncType<R, TArgs...>* orig_ptr = nullptr,
           uint16_t num_insts = kDefaultNumInsts, CallingConvention conv = CallingConvention::Cdecl,
           HookNameMetadata&& name_info =
               HookNameMetadata{
                 .name = "",
               },
           HookPriority&& priority =
               HookPriority{
                 .befores = {},
                 .afters = {},
               },
           bool is_midpoint = false)
      : target(target),
        orig_ptr(orig_ptr),
        hook_ptr(hook_func),
        metadata(HookMetadata{
          .convention = conv,
          .metadata =
              InstallationMetadata{
                .need_orig = orig_ptr != nullptr,
                .is_midpoint = is_midpoint,
              },
          .method_num_insts = num_insts,
          .name_info = name_info,
          .priority = priority,
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
          .parameter_info = { TypeInfo::from<TArgs>()... },
          .return_info = TypeInfo::from<R>(),
#endif
        }) {
  }

  void assign_orig(void* ptr) {
    if (orig_ptr != nullptr) *orig_ptr = ptr;
  }

  void* target;
  void** orig_ptr;
  void* hook_ptr;
  HookMetadata metadata;
};

}  // namespace flamingo
