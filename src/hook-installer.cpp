#include "hook-installer.hpp"
#include "enum-helpers.hpp"
#include <cassert>
#include "trampoline-allocator.hpp"

std::unordered_map<void*, HookTargetInstallation> HookInstaller::collected_hooks;
std::unordered_map<void*, std::list<void*>> HookInstaller::adjacency_map;

void HookInstaller::CreateAdjacencyMap() {
    // After we collected all hooks, create an adjacency map so we can do leapfrog hooks.
    // This is O(N^2) N being hooks that are small.
    // Log begin
    for (auto [target, hook] : collected_hooks) {
        if (hook.method_size < MinimumMethodSize) {
            // Oh no! We have a method that needs to be leapfrog hooked.
            // For now, we shall add an entry to the adjacency map.
            auto lst_pair = adjacency_map.insert({target, std::list<void*>()});
            for (auto [target2, hook2] : collected_hooks) {
                if (hook2.method_size > MinimumMethodSize + LeapfrogSize) {
                    // Space for a POTENTIAL leapfrog hook
                    // Need to make sure it's actually within an acceptable range, of course
                    // Nested leapfrogs may also need to apply
                    // Log additions here
                    lst_pair.first->second.push_back(target2);
                }
            }
        }
    }
    // Log completion
}

void HookInstaller::CollectHooks() {
    // Beginning collection!
    for (auto& hook : HookData::hooks_to_install) {
        // For each hook, resolve target
        auto target = hook.resolution_function();
        auto insertion = collected_hooks.try_emplace(target.target_method, target.target_method, hook, target.method_size);
        if (!insertion.second) {
            // Already exists, try to add the hook
            insertion.first->second.TryAddHook(hook, target);
        }
        // TODO: Consider if we need to add more metadata for hook installation smoothness?
        // Log what we are doing:
        // Collecting hook at: target, with target method size: size, with signature: retSize(paramSizes...), and metadata: metadata, with hook: ptr, trampoline: ptr
    }
    // Collection of hooks complete!
}

void HookInstaller::InstallConventionalHook(HookTargetInstallation& toInstall) {
    #ifndef FLAMINGO_NO_REGISTRATION_CHECKS
    assert(toInstall.registration_status == HookTargetInstallation::RegistrationStatus::Ok);
    #endif
    // The metadata for is_midpoint is actually not necessary here-- only whether we want an orig
    // If we do, we should allocate space for one. The space we allocate should be based off of two factors:
    // 1. The size of our hook in instructions
    // 2. If we need to perform a longer trampoline + fixups because we are a leapfrog destination
    // TODO: For now, we shall assume the first case alone.
    auto hookSize = ConventionalHookSize;
    // First hook is the one that is closest to the game code, that is, last executed
    // The last hook is the one that is actually written to the target destination, with each subsequent earlier hook
    // being the one that is called next, until the first hook is reached.
    // The first hook's orig either calls the allocated trampoline OR it does nothing

    std::size_t trampolineSize = hookSize;
    // Write actual hook to be a callback
    Trampoline targetHook(reinterpret_cast<uint32_t*>(toInstall.target), hookSize, trampolineSize);
    targetHook.WriteCallback(reinterpret_cast<uint32_t*>(toInstall.installation_info.back()->hook_ptr));
    // TODO: Write our remaining callbacks for leapfrog hooks
    targetHook.Finish();
    // Flush icache to update hook
    __builtin___clear_cache(reinterpret_cast<char*>(toInstall.target), reinterpret_cast<char*>(toInstall.target) + hookSize);
    // Log what we have done so far and also the rest of this
    for (auto revItr = toInstall.installation_info.rbegin(); revItr != toInstall.installation_info.rend();) {
        // hook_ptr is where we want to jump to FIRST.
        auto hook = *revItr;
        // orig ptr for this should forward to the hook_ptr for the next
        if (revItr++ != toInstall.installation_info.rend()) {
            *hook->orig_ptr = (*revItr)->hook_ptr;
        } else {
            // hook is the final hook.
            // Check to see if we want an orig, and if so, assign it
            if (toInstall.metadata.need_orig) {
                auto trampoline = TrampolineAllocator::Allocate(Trampoline::MaximumFixupSize * hookSize);
                // Write our fixups to the trampoline from our target address
                trampoline.WriteFixups(reinterpret_cast<uint32_t*>(toInstall.target), ConventionalHookSize / sizeof(uint32_t));
                *hook->orig_ptr = trampoline.address;
                toInstall.orig_trampoline.emplace(trampoline);
            } else {
                // TODO: Set the last hook's orig to some value that should never be called, to ensure nothing ever calls this.
                // This might also be handled by the macros.
            }
        }
    }
    // At this point, our hooks have all been installed!
    // We should mark this hook as installed
}

void HookInstaller::InstallHooks() {
    // Now, we are done collecting all hooks and have them in a better IR form.
    // We should first check our leapfrog hooks (adjacency map)
    #ifndef FLAMINGO_NO_LEAPFROG
    if (!adjacency_map.empty()) {
        // We have a NONZERO quantity of hooks that need to be leapfrogged.
        // All hooks that are in the dst of this list need to be managed.
        // TODO: For now, simply say that we can't handle this circumstance.
        // Log this
    }
    #endif
    // For each, we are going to have to do some stuff.
    for (auto [target, hook] : collected_hooks) {
        #ifndef FLAMINGO_NO_REGISTRATION_CHECKS
        if (hook.registration_status == HookTargetInstallation::RegistrationStatus::Ok) {
            // Installed OK!
            // Log what we are doing
        }
        else {
            if (flamingo::enum_helpers::HasFlag<HookTargetInstallation::RegistrationStatus::MismatchTargetConv>(hook.registration_status)) {
                // Log mismatch of target convention
            } if (flamingo::enum_helpers::HasFlag<HookTargetInstallation::RegistrationStatus::MismatchReturnSize>(hook.registration_status)) {
                // Log mismatch of sizes
            } if (flamingo::enum_helpers::HasFlag<HookTargetInstallation::RegistrationStatus::MismatchTargetParamSizes>(hook.registration_status)) {
                // Log mismatch of param sizes
            }
            // Continue even after noting this.
            // Log this
        }
        #endif

        if (hook.method_size >= MinimumMethodSize) {
            // This is a trivial example, we can just install :)
            // TODO: However, this should only be the case if we ALSO are not going to allocate space for a leapfrog destination.
            InstallConventionalHook(hook);
        } else {
            // TODO: Leapfrog install and determine validity
            // Log and skip this install
        }
    }
    // Log all hooks are installed!
}