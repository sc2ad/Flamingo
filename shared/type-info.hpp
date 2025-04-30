#pragma once

#include <cstddef>
#include <type_traits>

namespace flamingo {
/// @brief Represents the type info for representing a type in a hook
struct TypeInfo {
  friend struct HookInfo;
  // TODO: Add more members here. Ideally some form of relaxed type checking?
  std::size_t size{};

 private:
  template <class T>
  [[nodiscard]] inline static TypeInfo from() {
    if constexpr (std::is_reference_v<T>) {
      return TypeInfo{
        .size = sizeof(void*),
      };
    } else if constexpr (std::is_void_v<T>) {
      return TypeInfo{
        .size = 0,
      };
    } else {
      return TypeInfo{
        .size = sizeof(T),
      };
    }
  }
};
}  // namespace flamingo
