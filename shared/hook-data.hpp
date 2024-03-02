#pragma once
#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>
#include "calling-convention.hpp"
#include "hook-installation-result.hpp"
#include "trampoline.hpp"

namespace flamingo {

struct Hook;
struct TargetData;

struct InstallationMetadata {
  bool need_orig;
  bool is_midpoint;
  // uint32_t permissible_fixup_registers;

  installation::Result combine(InstallationMetadata const& other);
};

/// @brief Describes the name metadata of the hook, used for lookups and priorities.
/// Lookups are described using userdata when the HookInfo is made at first.
struct HookNameMetadata {
  std::string name{};
  std::any userdata{};
};

/// @brief Represents a priority for how to align hook orderings. Note that a change in priority MAY require a full list recreation.
/// But SHOULD NOT require a hook recompile or a trampoline recompile.
struct HookPriority {
  /// @brief The set of constraints for this hook to be installed before (called earlier than)
  std::vector<HookNameMetadata> befores{};
  /// @brief The set of constraints for this hook to be installed after (called later than)
  std::vector<HookNameMetadata> afters{};
};

struct HookMetadata {
  CallingConvention convention;
  InstallationMetadata metadata;
  uint16_t method_num_insts;
  HookNameMetadata name_info;
  HookPriority priority;
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
  std::vector<TypeInfo> parameter_info;
  TypeInfo return_info;
#endif
};

/// @brief Represents a hook that a user of this library will use.
/// On install, we collect this information into a single TargetInfo structure, which contains a collection of multiple Hook references.
/// We map target --> TargetInfo and every time we have a new hook installed there, we move orig pointers around accordingly.
/// To do priorities, we have to track that state within a given Hook
/// We don't necessarily need O(1) hook installation, but we could.
/// A TargetInfo may basically just be a Hook, except with the added list of Hooks, which we use to validate.
struct HookInfo {
  // TODO: friend struct this to something?
  friend struct TargetData;
  template <class R, class... TArgs>
  using HookFuncType = R (*)(TArgs...);
  // TODO: Could replace this with a single function, instead of hook by hook?
  // This function should return true if both userdata infos are equivalent.
  // This is to be used for modloader unaware code, but stateful hook infos
  using HookPriorityLookupFunction = std::function<bool(std::any const&, std::any const&)>;

  template <class R, class... TArgs>
  HookInfo(HookFuncType<R, TArgs...> hook_func, void* target, HookFuncType<R, TArgs...>* orig_ptr, uint16_t num_insts,
           CallingConvention conv, InstallationMetadata metadata, HookNameMetadata&& name_info, HookPriority&& priority,
           HookPriorityLookupFunction&& priority_lookup_func)
      : target(target),
        orig_ptr(orig_ptr),
        hook_ptr(hook_func),
        metadata(HookMetadata{
            .convention = conv,
            .metadata = metadata,
            .method_num_insts = num_insts,
            .name_info = name_info,
            .priority = priority,
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
            .parameter_info = { TypeInfo::from<TArgs>()... },
            .return_info = TypeInfo::from<R>(),
#endif
        }),
        priority_lookup_func(priority_lookup_func) {
  }

  void* target;

 private:
  void** orig_ptr;
  void* hook_ptr;
  HookMetadata metadata;
  HookPriorityLookupFunction priority_lookup_func;
};

/// @brief Represents the status of a particular address
/// If hooked, will contain the same members as a hook, but additionally with a list of Hooks
/// The idea being we can O(1) install hooks (and uninstall via iterator)
struct TargetData {
  HookMetadata metadata;
  std::optional<Trampoline> orig_trampoline{};
  std::list<HookInfo> hooks{};
  Trampoline target;

  installation::Result combine(HookInfo&& incoming);
};

/// @brief To install a hook, we require a constructed HookInfo. We want the value to be thrown away, so we require an rvalue (we may also
/// forward params?). Because a HookInfo is just data, we go find our TargetInfo that matches our target.
/// Then, we attempt to install that HookInfo onto the TargetData, mutating the TargetData (but not invalidating other HookInfo referneces
/// within the list). We update the shared information within the HookInfo and perform the install as necessary.
/// The order of priorities is: lowest value runs before higher values.
/// TODO: Make priorities potentially use named IDs for cleaer ordering (before x, after y). This may require a full reassmebly of the list!
auto Install(HookInfo&& hook);
}  // namespace flamingo
