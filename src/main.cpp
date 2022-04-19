// On dlopen, we should basically just construct our vectors and everything else
// As well as our analytics data
#include "more_stuff.hpp"
#include "hook-installer.hpp"
// #include "hook-installer.hpp"
#include "modloader/shared/modloader.hpp"
// #include <type_traits>
#include <cassert>
#include "enum-helpers.hpp"

// Error reporter for pseudo-enum types
namespace {
    template<class T, T val>
    struct ErrorReporter;

    #define REPORT_ERROR(Type, Value, msg) \
    template<> \
    struct ErrorReporter<Type, Type::Value> { \
        static void ReportError(Type value, HookTargetInstallation& existing, HookData& incoming) { \
            if ((static_cast<int>(value) & static_cast<int>(Type::Value)) != 0) { \
                /* TODO: printf("Error registering hook! Error: " msg "\n"); */ \
            } \
        } \
    }

    REPORT_ERROR(HookTargetInstallation::RegistrationStatus, MismatchTargetConv, "Mismatched target calling convention!");
    REPORT_ERROR(HookTargetInstallation::RegistrationStatus, MismatchTargetParamSizes, "Mismatched target parameter sizes!");
    REPORT_ERROR(HookTargetInstallation::RegistrationStatus, MismatchReturnSize, "Mismatched return size!");

    template<HookTargetInstallation::RegistrationStatus value>
    concept has_error_reporter = requires (HookTargetInstallation& existing, HookData& incoming) {
        ErrorReporter<HookTargetInstallation::RegistrationStatus, value>::ReportError(value, existing, incoming);
    };

    template<HookTargetInstallation::RegistrationStatus... Errors>
    requires (has_error_reporter<Errors> && ...)
    void ReportErrors(HookTargetInstallation::RegistrationStatus value, HookTargetInstallation& existing, HookData& incoming) {
        (ErrorReporter<HookTargetInstallation::RegistrationStatus, Errors>::ReportError(value, existing, incoming), ...);
    }
}

std::list<HookData> HookData::hooks_to_install;

void HookData::RegisterHook(HookData&& data) {
    hooks_to_install.emplace_back(std::forward<HookData>(data));
}

extern "C" void load() {
    // Here's where we will INSTALL all of our hooks!
    HookInstaller::CollectHooks();
    #ifndef FLAMINGO_NO_LEAPFROG
    HookInstaller::CreateAdjacencyMap();
    #endif
    HookInstaller::InstallHooks();
}