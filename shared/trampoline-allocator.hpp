#pragma once

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <vector>
// #include "beatsaber-hook/shared/utils/capstone-utils.hpp"

struct Trampoline {
    // In bytes for a single fixup
    constexpr static uint16_t MaximumFixupSize = 20;

    uint32_t* address;
    std::size_t alloc_size;
    // size is number of instructions
    std::size_t instruction_count;
    std::size_t& pageSizeRef;
    std::vector<uint32_t> original_instructions;

    Trampoline(uint32_t* ptr, std::size_t allocationSize, std::size_t& sz) : address(ptr), alloc_size(allocationSize), pageSizeRef(sz) {}

    void Write(uint32_t instruction);
    void Write(void const* ptr);
    void WriteCallback(uint32_t const* target);
    void WriteB(int64_t imm);
    void WriteBl(int64_t imm);
    void WriteAdr(uint8_t reg, int64_t imm);
    void WriteAdrp(uint8_t reg, int64_t imm);

    /// @brief Helper function to write:
    /// LDR x17, #0x8
    /// BR X17
    /// DATA
    void WriteLdrBrData(uint32_t const* target);
    void WriteFixup(uint32_t const* target);
    void WriteFixups(uint32_t const* target, uint16_t countToFixup);
    /// @brief Logs various information about the trampoline.
    void Log();
    /// @brief A TRAMPOLINE IS NOT COMPLETE UNTIL FINISH IS CALLED!
    void Finish();
};

struct TrampolineAllocator {
    static Trampoline Allocate(std::size_t trampolineSize);
    static void Free(Trampoline const& toFree);
};