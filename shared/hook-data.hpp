#pragma once
#include <cstdint>
#include "calling_convention.hpp"
#include <list>
#include <vector>

struct TargetData {
    void* target_method;
    uint16_t method_size;
    CallingConvention calling_convention;
};

struct InstallationMetadata {
    uint8_t need_orig : 1;
    uint8_t is_midpoint : 1;
    // uint32_t permissible_fixup_registers;
};

struct HookData {
    using TargetResolutionPtr = TargetData (*)();
    friend struct HookInstaller;

    TargetResolutionPtr resolution_function;
    std::vector<std::size_t> parameter_sizes;
    std::size_t return_size;
    void** orig_ptr;
    void* hook_ptr;
    CallingConvention convention;
    InstallationMetadata metadata;

    static void RegisterHook(HookData&& d);
    private:
    static std::list<HookData> hooks_to_install;
};