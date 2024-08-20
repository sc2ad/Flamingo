#pragma once
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "calling-convention.hpp"
#include "target-data.hpp"
#include "type-info.hpp"

namespace flamingo {

template <class T, class E>
struct Result {
  std::variant<E, T> data;
  template <class... TArgs>
  static Result Ok(TArgs&&... args) {
    return Result{ std::in_place_index_t<1>{}, std::forward<TArgs>(args)... };
  }
  template <class... TArgs>
  static Result Err(TArgs&&... args) {
    return Result{ std::in_place_index_t<0>{}, std::forward<TArgs>(args)... };
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

using Error = std::tuple<std::optional<MismatchReturn>, std::optional<MismatchParam>, std::optional<MismatchTargetConv>,
                         std::optional<MismatchMidpoint>>;

using Result = std::variant<Ok, Error>;

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

inline Error& operator|=(Error& lhs, Error const& rhs) {
  // Merge rhs into lhs for every non-nullopt in rhs
  std::apply(
      [&lhs](auto&&... values) {
        (merge_optional(std::get<std::remove_cvref_t<decltype(values)>>(lhs), std::forward<decltype(values)>(values)),
         ...);
      },
      rhs);
  return lhs;
}

inline Result& operator|=(Result& lhs, Result&& rhs) {
  // TODO: Figure out a good way of merging stuff here
  // TODO: WHY ARE WE DOING IT THIS WAY
  if (std::holds_alternative<Ok>(rhs)) {
    lhs.emplace<Ok>(std::get<Ok>(rhs));
  } else {
    lhs.emplace<Error>(std::get<Error>(rhs));
  }
  return lhs;
}

}  // namespace installation

}  // namespace flamingo
