#pragma once

#include <stdint.h>

#define FLAMINGO_C_EXPORT __attribute__((visibility("default")))
// The flamingo C API

#ifdef __cplusplus
extern "C" {
#endif
// General C API:
// Create a hook (get back a handle)
// Uninstall/remove a hook (with the handle)

// Creation of the hook is actually a non-trivial process.
// We require lots of meta information, potentially.

// Check if a location is hooked
// Additionally return a window over the original data (to allow for xref traces)
// Reinstall all hooks at a target, or a range

/// @brief Returned from a call to query if a region is hooked, and what the original instructions at that location are.
typedef struct {
  /// @brief The size of the hook present at this address, in number of instructions.
  uint32_t hook_size;
  /// @brief A pointer to the hook's original instructions. Safe to dereference up to 'hook_size'.
  uint32_t const* original_instructions;
} FlamingoOriginalInstructionsResult;

/// @brief Returns a FlamingoOriginalInstructionsResult.
/// The hook_size of the returned instance will be 0 if the provided address is not the START of an installed hook.
/// The returned original_instructions pointer is safe to read in the range [0..hook_size).
/// This function is commonly used for ensuring correct results when going through an xref trace, or validating real
/// instructions.
FLAMINGO_C_EXPORT FlamingoOriginalInstructionsResult flamingo_orig_for(uint32_t const* addr);



#ifdef __cplusplus
}
#endif
