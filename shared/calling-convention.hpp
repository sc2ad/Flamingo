#pragma once

namespace flamingo {
/// @brief Represents the calling convention for a given hook.
/// Used primarily for type checking
enum struct CallingConvention { Cdecl, Fastcall, Thiscall };
}  // namespace flamingo
