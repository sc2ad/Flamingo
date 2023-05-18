#pragma once
#include "calling_convention.hpp"
#include <unordered_map>
#include <vector>
#include <list>
#include <optional>
#include "hook-data.hpp"
#include "trampoline-allocator.hpp"

struct HookTargetInstallation {
    friend struct HookInstaller;

    #ifndef FLAMINGO_NO_REGISTRATION_CHECKS
    enum struct RegistrationStatus {
        None = 0,
        Ok = 1,
        MismatchTargetConv = 2,
        MismatchTargetParamSizes = 4,
        MismatchReturnSize = 8,
        MismatchMetadataMidpoint = 16,
    };
    private:
    RegistrationStatus registration_status;
    std::vector<std::size_t> parameter_sizes;
    std::size_t return_size;
    public:
    HookTargetInstallation(void* target_, HookData& from, uint16_t size) : parameter_sizes(from.parameter_sizes), return_size(from.return_size), target(target_), calling_convention(from.convention), method_size(size), metadata(from.metadata) {
        installation_info.push_back(&from);
    }
    #else
    HookTargetInstallation(void* target_, HookData& from, uint16_t size) : target(target_), calling_convention(from.convention), method_size(size), metadata(from.metadata) {
        installation_info.push_back(&from);
    }
    #endif
    void TryAddHook(HookData& toAdd, TargetData const& target);

    private:
    void* target;
    std::vector<HookData*> installation_info;
    CallingConvention calling_convention;
    uint16_t method_size;
    InstallationMetadata metadata;
    std::optional<Trampoline> orig_trampoline;
};

struct HookInstaller {
    static void CollectHooks();
    static void CreateAdjacencyMap();
    static void InstallHooks();

    private:
    static void InstallConventionalHook(HookTargetInstallation& toInstall);

    // In bytes
    constexpr static uint16_t MinimumMethodSize = 20;
    // In bytes
    constexpr static uint16_t LeapfrogSize = 16;
    // In bytes
    constexpr static uint16_t ConventionalHookSize = 16;
    static std::unordered_map<void*, HookTargetInstallation> collected_hooks;
    #ifndef FLAMINGO_NO_LEAPFROG
    static std::unordered_map<void*, std::list<void*>> adjacency_map;
    static void InstallLeapfrogHook(HookTargetInstallation const& toInstall, std::list<void*> const& adjacencies);
    #endif
};
