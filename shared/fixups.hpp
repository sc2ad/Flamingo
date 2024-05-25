#pragma once

#include <capstone/capstone.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "page-allocator.hpp"
#include "util.hpp"

namespace flamingo {

template <class T>
struct ProtectionWriter {
  // The target to write to
  PointerWrapper<T> target;
  PageProtectionType original_permissions;
  // Where in the target we are currently about to write to
  uint_fast16_t target_offset{ 0 };

  ProtectionWriter(PointerWrapper<T> ptr) : target(ptr), original_permissions(target.protection) {
    // When we construct this writer, we mark the page we are operating on as writable.
    // To do this, we align the pointer down to the multiple of the PageSize
    // and then we protect it with the write permission.
    target.protection |= PageProtectionType::kWrite;
    target.Protect();
  }
  ProtectionWriter(ProtectionWriter const&) = delete;
  ProtectionWriter(ProtectionWriter&& other)
      : target(other.target), original_permissions(other.original_permissions), target_offset(other.target_offset) {
    // Set other's target to null to avoid a case where we re-protect on the first instance's dtor
    other.target.addr = {};
  }

  ~ProtectionWriter() {
    target.protection = original_permissions;
    target.Protect();
  }
  // Write data to this writer. Returns the index that we wrote to.
  uint_fast16_t Write(T inst) {
    if (target_offset >= target.addr.size()) {
      FLAMINGO_ABORT("Cannot write if there is no space available! {} should be < {}", target_offset,
                     target.addr.size());
    }
    target.addr[target_offset] = inst;
    auto to_return = target_offset;
    target_offset++;
    return to_return;
  }
};

struct Fixups {
  // The location to read as input for fixup writes
  PointerWrapper<uint32_t> target;
  // The location to write fixups to
  PointerWrapper<uint32_t> fixup_inst_destination;
  std::vector<uint32_t> original_instructions{};

  /// @brief Logs various information about the fixups.
  /// Will log the original instructions and the full set of fixups, including data, for the full allocation window
  void Log() const;
  // For the input target, walks over the size passed in to the span
  // For each instruction listed, fixes it up
  void PerformFixupsAndCallback();
};

// TODO: DO NOT EXPOSE THIS SYMBOL (USE IT FOR TESTING ONLY)
csh getHandle();

}  // namespace flamingo