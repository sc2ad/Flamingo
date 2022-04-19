#pragma once
/// @brief Represents the calling convention for a given hook.
enum struct CallingConvention {
    Cdecl,
    Fastcall,
    Thiscall
};