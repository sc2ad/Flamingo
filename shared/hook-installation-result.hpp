#pragma once
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "calling-convention.hpp"
#include "hook-metadata.hpp"
#include "target-data.hpp"
#include "type-info.hpp"

namespace flamingo {

template<class T>
struct is_variant {
  constexpr static bool value = false;
};

template<class... TArgs>
struct is_variant<std::variant<TArgs...>> {
  constexpr static bool value = true;
};

template <class T, class E>
struct Result {
  std::variant<E, T> data;
  template <class... TArgs>
  static Result Ok(TArgs&&... args) {
    return Result{ std::variant<E, T>(std::in_place_index_t<1>{}, std::forward<TArgs>(args)...) };
  }
  template <class... TArgs>
  static Result Err(TArgs&&... args) {
    return Result{ std::variant<E, T>(std::in_place_index_t<0>{}, std::forward<TArgs>(args)...) };
  }
  // Helper function for if E is a variant (we have multiple errors and need to construct one)
  template<class ET, class... TArgs>
  requires (is_variant<E>::value)
  static Result ErrAt(TArgs&&... args) {
    return Result{ std::variant<E, T>(std::in_place_index_t<0>{}, E(std::in_place_type_t<ET>{}, std::forward<TArgs>(args)...))};
  }
  T const& value() const {
    return std::get<1>(data);
  }
  E const& error() const {
    return std::get<0>(data);
  }
  bool has_value() const {
    return data.index() == 1;
  }
};

namespace installation {

/// @brief Holds metadata about the successful install
struct Ok {
  HookHandle returned_handle;
};

/// @brief The general base type for reporting hook errors. Holds the ID of the failing hook.
struct HookErrorInfo {
  HookNameMetadata installing_hook;
  HookErrorInfo(HookNameMetadata const& m) : installing_hook(m) {}
};

/// @brief An error when the target of a hook install is null.
struct TargetIsNull : HookErrorInfo {
  TargetIsNull(HookNameMetadata const& m) : HookErrorInfo(m) {}
};
/// @brief An error when the target method is described as too small for the hook strategy being employed.
struct TargetTooSmall : HookErrorInfo {
  TargetTooSmall(HookMetadata const& m, uint_fast16_t needed)
      : HookErrorInfo(m.name_info), actual_num_insts(m.method_num_insts), needed_num_insts(needed) {}
  uint_fast16_t actual_num_insts;
  uint_fast16_t needed_num_insts;
};
/// @brief An error when the target method is impossible to install given its priorities and other hooks to install it onto.
struct TargetBadPriorities : HookErrorInfo {
  // TODO: Add a bunch of stuff here
  TargetBadPriorities(HookMetadata const& m) : HookErrorInfo(m.name_info) {}
};
/// @brief An error when the target method has some validation failure with respect to the metadata it holds.
struct TargetMismatch : HookErrorInfo {
  TargetMismatch(HookMetadata const& m) : HookErrorInfo(m.name_info) {}
};

// TODO: Should we add the incoming hook IDs?

struct MismatchReturn {
  TypeInfo existing{};
  TypeInfo incoming{};
};

struct MismatchParam {
  size_t idx{};
  TypeInfo existing{};
  TypeInfo incoming{};
};

struct MismatchTargetConv {
  CallingConvention existing{};
  CallingConvention incoming{};
};

struct MismatchMidpoint {
  bool existing{};
  bool incoming{};
};

namespace util {

template <typename T, typename... TArgs>
consteval static bool all_unique() {
  if constexpr (sizeof...(TArgs) == 0ULL) {
    return true;
  } else {
    return (!std::is_same_v<T, TArgs> && ...) && all_unique<TArgs...>();
  }
}

template <class... TArgs>
  requires(all_unique<TArgs...>())
using OptionTuple = std::tuple<std::optional<TArgs>...>;

template <class... TArgs>
  requires(all_unique<TArgs...>())
struct OptionalErrors {
 private:
  OptionTuple<TArgs...> maybe_errors;

 public:
  [[nodiscard]] constexpr bool has_error() const {
    return std::apply([](auto&&... opts) { return (opts || ...); }, maybe_errors);
  }
  template <class T>
  void assign(T&& val) {
    std::get<std::optional<T>>(maybe_errors) = std::forward<T>(val);
  }
};

}  // namespace util

// Can be one of many cases.
using Error = std::variant<TargetIsNull, TargetBadPriorities, TargetMismatch, TargetTooSmall>;

using Result = flamingo::Result<Ok, Error>;

template <class T>
auto& get(Error const& obj) {
  return std::get<std::optional<T>>(obj);
}

template <class T>
void merge_optional(std::optional<T>& lhs, std::optional<T> const& rhs) {
  if (rhs) {
    lhs.emplace(*rhs);
  }
}

}  // namespace installation

}  // namespace flamingo
