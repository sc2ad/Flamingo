#include "hook-installer.hpp"
#include <cassert>
#include "enum-helpers.hpp"

void HookTargetInstallation::TryAddHook(HookData& toAdd, TargetData const& target)
{
    assert(!installation_info.empty());
    // Only need to check the back of the collection to ensure validity.
    installation_info.push_back(&toAdd);

    #ifndef FLAMINGO_NO_REGISTRATION_CHECKS
    HookTargetInstallation::RegistrationStatus status = HookTargetInstallation::RegistrationStatus::None;
    if (calling_convention != target.calling_convention) {
        status = flamingo::enum_helpers::AddFlag<HookTargetInstallation::RegistrationStatus::MismatchTargetConv>(status);
    }
    if (return_size != toAdd.return_size) {
        status = flamingo::enum_helpers::AddFlag<HookTargetInstallation::RegistrationStatus::MismatchReturnSize>(status);
    }
    if (parameter_sizes.size() == toAdd.parameter_sizes.size()) {
        auto itr1 = parameter_sizes.begin();
        auto itr2 = toAdd.parameter_sizes.begin();
        for (; itr2 != toAdd.parameter_sizes.end(); itr1++, itr2++) {
            if (*itr1 != *itr2) {
                status = flamingo::enum_helpers::AddFlag<HookTargetInstallation::RegistrationStatus::MismatchTargetParamSizes>(status);
                break;
            }
        }
    } else {
        status = flamingo::enum_helpers::AddFlag<HookTargetInstallation::RegistrationStatus::MismatchTargetParamSizes>(status);
    }
    // If we think of ourselves as being a midpoint hook, yet someone else disagrees, this is a problem
    if (metadata.is_midpoint != toAdd.metadata.is_midpoint) {
        status = flamingo::enum_helpers::AddFlag<HookTargetInstallation::RegistrationStatus::MismatchMetadataMidpoint>(status);
    }
    if (metadata.need_orig != toAdd.metadata.need_orig) {
        // TODO: Determine how we want to handle this case, specifically where someone wants orig but someone else doesn't (is a rewrite)
        // Should we have the non-orig be the top level and call it a day?
        // - Thus should we only complain if we have TWO cases where that is not the case?
        // For now, we will just not handle this any differently and probably just excessively write an orig
    }

    // Use the most restrictive set of fixup registers possible
    // metadata.permissible_fixup_registers &= toAdd.metadata.permissible_fixup_registers;

    if (status == HookTargetInstallation::RegistrationStatus::None) {
        // Success until this point
        status = HookTargetInstallation::RegistrationStatus::Ok;
    }
    registration_status = status;
    if (target.method_size < method_size) {
        // Method size is the minimum known method size for this location
        method_size = target.method_size;
    }
    #endif
}
