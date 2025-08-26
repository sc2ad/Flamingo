#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "calling-convention.hpp"
#include "type-info.hpp"

namespace flamingo {
struct Hook;
struct TargetData;

struct InstallationMetadata {
  bool need_orig;
  bool is_midpoint;
  /// @brief If write protection should be enabled for the target address (primarily for debugging to avoid issues with near pages)
  bool write_prot;
};

/// @brief Describes the name metadata of the hook, used for lookups and priorities.
/// Lookups are described using userdata when the HookInfo is made at first.
struct HookNameMetadata {
  std::string name{};
};

/// @brief Represents a priority for how to align hook orderings. Note that a change in priority MAY require a full list
/// recreation. But SHOULD NOT require a hook recompile or a trampoline recompile.
struct HookPriority {
  /// @brief The set of constraints for this hook to be installed before (called earlier than)
  std::vector<HookNameMetadata> befores{};
  /// @brief The set of constraints for this hook to be installed after (called later than)
  std::vector<HookNameMetadata> afters{};
};

struct HookMetadata {
  CallingConvention convention;
  InstallationMetadata installation_metadata;
  uint16_t method_num_insts;
  HookNameMetadata name_info;
  HookPriority priority;
#ifndef FLAMINGO_NO_REGISTRATION_CHECKS
  std::vector<TypeInfo> parameter_info;
  TypeInfo return_info;
#endif
};

}  // namespace flamingo