#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace flamingo {

struct Trampoline {
    std::span<uint32_t> address;
    // size is number of instructions
    // TODO: Make a transparent type here to avoid accidental comparison with other types of size
    std::size_t num_insts;
    std::size_t instruction_count = 0;
    std::size_t& pageSizeRef;
    std::vector<uint32_t> original_instructions{};

    Trampoline(uint32_t* ptr, std::size_t num_insts, std::size_t& sz)
        : address(ptr, &ptr[num_insts]), num_insts(num_insts), pageSizeRef(sz) {}

    void Write(uint32_t instruction);
    /// @brief Writes the specified pointer as a target specific immediate to the data block.
    /// @param ptr The pointer to write as a raw literal piece of data. NOT dereferenced.
    void WriteData(void const* ptr);
    /// @brief Performs a memcpy from the specified pointer and size
    /// @param ptr The pointer to memcpy from
    /// @param size The number of 4 byte words to copy
    void WriteData(void const* ptr, uint32_t size);
    void WriteCallback(uint32_t const* target);
    void WriteB(int64_t imm);
    void WriteBl(int64_t imm);
    void WriteAdr(uint8_t reg, int64_t imm);
    void WriteAdrp(uint8_t reg, int64_t imm);
    void WriteLdr(uint32_t inst, uint8_t reg, int64_t imm);

    /// @brief Helper function to write:
    /// LDR x17, #0x8
    /// BR X17
    /// DATA
    void WriteLdrBrData(uint32_t const* target);
    // TODO: Put this in an internal-trampoline header instead so that it can be used from other TUs but is not exposed
    template <uint16_t countToFixup>
    void WriteFixups(uint32_t const* target);
    void WriteHookFixups(uint32_t const* target);
    /// @brief Logs various information about the trampoline.
    void Log();
    /// @brief A TRAMPOLINE IS NOT COMPLETE UNTIL FINISH IS CALLED!
    void Finish();
};

}  // namespace flamingo