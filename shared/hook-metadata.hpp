#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <fmt/format.h>
#include <fmt/compile.h>

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
  std::string namespaze{};

  [[nodiscard]]
  bool operator==(HookNameMetadata const& other) const {
    return (name == other.name) && (namespaze == other.namespaze);
  }
};

/// Specifies the filter type for hook names in priorities
struct HookNameFilter {
  std::optional<std::string> namespaze{};
  std::optional<std::string> name{};

  explicit HookNameFilter() = default;
  explicit HookNameFilter(std::string namespaze) : namespaze(std::move(namespaze)) {}
  explicit HookNameFilter(std::string namespaze, std::string name) : namespaze(std::move(namespaze)), name(std::move(name)) {}

  /// @brief Construct a filter that matches the provided metadata. This is used for constructing filters from userdata.
  HookNameFilter(HookNameMetadata const& metadata) : namespaze(metadata.namespaze), name(metadata.name) {}

  /// @brief Checks if the provided metadata matches this filter. 
  // A filter with no fields set matches everything.
  [[nodiscard]] bool matches(HookNameMetadata const& metadata) const {
    if (name.has_value() && name.value() != metadata.name) {
      return false;
    }
    if (namespaze.has_value() && namespaze.value() != metadata.namespaze) {
      return false;
    }
    return true;
  }
};

/// @brief Represents a priority for how to align hook orderings. Note that a change in priority MAY require a full list
/// recreation. But SHOULD NOT require a hook recompile or a trampoline recompile.
struct HookPriority {
  /// @brief The set of constraints for this hook to be installed before (called earlier than)
  std::vector<HookNameFilter> befores{};
  /// @brief The set of constraints for this hook to be installed after (called later than)
  std::vector<HookNameFilter> afters{};
  /// @brief Set to true if this hook should be the final hook (closest to the original function)
  bool is_final{false};
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

// Custom formatter for flamingo::HookNameMetadata
template <>
class fmt::formatter<flamingo::HookNameMetadata> {
 public:
  constexpr auto parse(format_parse_context& ctx) {
    return ctx.begin();
  }
  template <typename Context>
  constexpr auto format(flamingo::HookNameMetadata const& metadata, Context& ctx) const {
    return fmt::format_to(ctx.out(), "name: {} namespace {}", metadata.name, metadata.namespaze);
  }
};
template <>
class fmt::formatter<flamingo::HookNameFilter> {
 public:
  constexpr auto parse(format_parse_context& ctx) {
    return ctx.begin();
  }
  template <typename Context>
  constexpr auto format(flamingo::HookNameFilter const& filter, Context& ctx) const {
    return fmt::format_to(ctx.out(), "name: {} namespace {}", filter.name.value_or("*"),
                          filter.namespaze.value_or("*"));
  }
};

// HookNameMetadata hash
namespace std {
template <>
struct hash<flamingo::HookNameMetadata> {
  std::size_t operator()(flamingo::HookNameMetadata const& k) const {
    return std::hash<std::string>()(k.name) ^ (std::hash<std::string>()(k.namespaze) << 1);
  }
};

}  // namespace std