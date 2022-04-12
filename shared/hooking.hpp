#pragma once
#include <string_view>
#include <array>
#include <concepts>
#include <unordered_map>
#include <vector>
#include <span>
#include "util.hpp"
#include <set>

// What do we want the syntax to look like?
// Remember, we need to provide a way of having priorities/before/after
// 
// We could make something like: DelayHook([](){address to install to})
// And also have something like: ImmediateHook<addrToInstall>
// And also: ImmediateHook(address to install to immediately)
// Would love to also provide information for before/after calls
// Templates for that would be _ideal_ but not _mandatory_
// Ex: Before<"one", "two">, After<"three", "four">
// Because we can't promote string literals to types (hecc you ndk) we may have some more trouble (as in, we would have to do runtime conversions to compare)
// Turns out clang sucks absolute ass and won't even let us use ANY type of non-template classes that aren't integral types.
// Sooooooo we are SOL from a constexpr perspective, without a ton of macros and all constexpr+virtual

// Before and After types SHOULD use string literals to avoid ambiguity.

template<class... TArgs>
requires ((std::is_convertible_v<TArgs, const char*> && ...))
constexpr auto Before(TArgs... strs) {
    return make_array(strs...);
}
template<class... TArgs>
requires ((std::is_convertible_v<TArgs, const char*> && ...))
constexpr auto After(TArgs... strs) {
    return make_array(strs...);
}

struct Priority {
    constexpr explicit Priority(uint32_t p) : priority(p) {}
    uint32_t priority;
};

struct HookCreatorIl2Cpp {
    auto getData() {
        // Check the function signature make sure it is valid for our target installation
        return Data(functionPtr);
    }
};

struct TargetType {
    void* dst;
    std::size_t methodSize;
};

struct Data {
    // This should be a function pointer
    // Which should return a Target type
    void* targetInstallation;
    // To perform checks on hook installs, make sure everything matches
    // TODO: Maybe unnecessary

    std::vector<std::size_t> paramSizes;
    std::size_t returnSize;
    void* hookAddress;
    private:
    TargetType target;
};

/*

Hook1
--> Hook2
    --> Hook3
Uninstall Hook2:
- Take hook1's orig, assign it to Hook3
- ACTUALLY uninstall hook2:
-- actually is done, we just move the pointers and call it a day.
-- IF hook2 was the ONLY OR LAST hook, things are a bit trickier:
--- Last hook is the one that calls actual orig
--- Last hook removal:
--- Take orig from prev, assign it to true orig pointer
--- great!
-- Else, hook2 is ONLY:
--- Replace hooked asm with original
--- deallocate fixups/true orig
- Update the tracking metadata
*/

// Uninstall hooks:
// - Pass in a method pointer to MY hooked method that I want uninstalled, perhaps also dst?

std::vector<Data> hooks;

struct Hook {
    void* target;
    Data install() {
        auto lambda = [](Hook* self) {
            self->target = (void*)0x12345678;
            return TargetType{(void*)0x12345678, 0};
        };
        return Data{&std::bind(lambda, this)};
    }
    static auto hook(int x, int y) {}
    void uninstall() {
        // Travel through hook collection
        // Look at hook addresses
        // Find a match to hook function
        auto found = std::find_if(hooks.begin(), hooks.end(), [](Data& h) {return h.hookAddress == &hook;});
        if (found != hooks.end()) {
            // Uninstall the hook
            found->target
        }
    }
};

// Make sure hooks don't try to install within each other
// If target is at 0x0, cannot install until 0x14 (probably)

struct HookInstaller {
    // Holds the collection of hooks that need to be installed, specifically their metadata and where to install them (where applicable)
    // When we install a hook, we should also provide a known method size
    // Installing hooks should assume that MOST hooks are already provided for us
    // We need to install in order, we may even want to force prefix/postfix
    // Handler should be:
    // Prefixes in correct order
    // orig --> ret to temp
    // Postfixes in correct order
    // ret with temp
};

// Holds the metadata required for a hook
struct HookMetadata {
    // The address to install this particular hook
    virtual void* addr() = 0;
    // The function to jump to during fire
    virtual void* function() = 0;
    virtual std::span<const char*> befores() = 0;
    virtual std::span<const char*> afters() = 0;
    virtual Priority priority() = 0;
    virtual std::string id() = 0;
};

template<typename From, typename To>
concept convertible_to = std::is_convertible_v<From, To>;

template<typename T>
concept is_valid_metadata_getter = requires (T t) {
    {t()} -> convertible_to<HookMetadata*>;
};

// Pass in a metadata getter function, which we invoke (either early, or on each pass) to determine metadata
template<auto metadataGetterFunc, typename R, typename... TArgs>
requires (is_valid_metadata_getter<decltype(metadataGetterFunc)>)
struct HookHandler {
    // This will be what we jump to when we perform any hook.
    // However, we need to properly have data about WHERE this hook is, what hooks we have, etc.
    // We could push something to the top of stack that tells us our hook address/any other metadata we need
    // But it is a bit unfortunate, since we could probably get the metadata in a more clever way.

    // The overall hook handler should get the metadata, then invoke each hook with metadata at the top of stack
    // Each hook will have a prologue to take the top thing off of stack and put it into a non-volatile reg
    // Then we take that value and assume it is metadata, which it is.
    // That way, when we go to invoke our "orig" we actually just call the next thing in line.
    // We could also solve this by dynamically changing what our orig is depending on hook installation
    static R handler(TArgs... args) {
        HookMetadata* metadata = metadataGetterFunc();
        // Iterate hooks for this particular section, fire all prefixes
        // Then fire for orig, save ret to temp if non-void
        // Then fire postfixes, provide ret as reference
        // return ret, if needed
    }
    // We could also perform this differently depending on if we actually NEED our hook to be a b, though it makes the most immediate sense
    // We could also just perform another level of indirection, though that doesn't really solve anything, per-se.
};

struct HookManager {

    static std::unordered_map<uint32_t*, std::vector<HookMetadata*>> hooks;

    static void sort(std::vector<HookMetadata*>& hooks) {

        std::sort(hooks.begin(), hooks.end(), [](HookMetadata* first, HookMetadata* second) {
            // Return true if first should come before second
            auto& firstId = first->id();
            auto& secondId = second->id();
            return first->id() == second->befores().
        });
    }
    static void recompile(uint32_t* target, uint32_t methodSize = -1) {
        // Recompiles all hooks that are at this location.
        // A recompile consists of the following:
        // - Collection of all hooks at this location
        auto targetHooksItr = hooks.find(target);
        if (targetHooksItr == hooks.end()) {
            // No hooks means we just exit
            return;
        }
        auto& targetHooks = targetHooksItr->second;
        // - Sorted while respecting priorities
        std::sort(targetHooks.begin(), targetHooks.end(), [](HookMetadata* first, HookMetadata* second) {
            return first-
        });
        // - Trampoline origs are recompiled in sorted order
        // - Actual target is recompiled with jump to first hook, last hook's orig refers back to target
        // - Optimizations may take place-- ex, leapfrog if known hooks are close enough
        std::multiset<int>::iterator
    }
};

// Hooks could hold their own trampoline, though what we put in it should be handled by the thing that orders the various hooks

struct Hook {
    // Before/After hooks apply BEFORE priority
    template<size_t szBefore = 0, size_t szAfter = 0>
    explicit Hook(std::string_view name, std::array<const char*, szBefore> before = std::array<const char*, 0>(), std::array<const char*, szAfter> after = std::array<const char*, 0>()) {

    }
    // Priority specific hooks apply AFTER before/after hooks, are also unique from before/after construction
    explicit Hook(std::string_view name, Priority p) {

    }
};

Hook q("Hi There!", Before("you", "him", "her"), After("Me"));

Hook two("Test", Priority(10));

