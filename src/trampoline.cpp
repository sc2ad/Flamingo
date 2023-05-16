#include "trampoline.hpp"
#include <cstring>
#include "capstone/capstone.h"
#include "capstone/platform.h"
#include "fmt/format.h"
#include "util.hpp"

namespace flamingo {

// TODO: We should consider an optimization where we have a location for fixup data instead of inling all fixups.
// This would allow us to write out actual assembly verbatim and then have ldrs and whatnot for grabbing the data
// This would save a few instructions per all of the fixups, since we wouldn't need to branch over the data
// In addition, it would allow us to have a better time disassembling.
// It also shouldn't cost us any space whatsoever
// Though, if we perform any hooks AFTER the fact, we would need to properly expand both our data and our instruction space
// and THAT could be somewhat tricky.
// Perhaps a full recompile for a hook is actually preferred, though, if we know we need to leapfrog
// Since we would need to expand our trampoline and our original instructions regardless.
// TODO: Consider a full recompile and permit late installations

/// @brief Helper function that returns an encoded b for a particular offset
consteval uint32_t get_b(int offset) {
    constexpr uint32_t b_opcode = 0b00010100000000000000000000000000U;
    return (b_opcode | (offset >> 2));
}

constexpr int64_t get_untagged_pc(uint64_t pc) {
    // Upper byte is tagged for PC addresses on android 11+
    constexpr uint64_t mask = ~(0xFFULL << 56);
    return static_cast<int64_t>(static_cast<uint64_t>(pc) & mask);
}

void Trampoline::Write(uint32_t instruction) {
    FLAMINGO_ASSERT((instruction_count + 1) * sizeof(uint32_t) <= alloc_size);
    // Log what we are writing (and also our state)
    address[instruction_count] = instruction;
    instruction_count++;
}

void Trampoline::WriteData(void const* ptr) {
    // TODO: Write this to a different buffer and return a pointer to it
    // This would allow the control flow for a hook to be much cleaner and the data section to be well defined
    FLAMINGO_ASSERT(instruction_count * sizeof(uint32_t) + sizeof(void*) <= alloc_size);
    // Log what we are writing (and also our state)
    *reinterpret_cast<void**>(&address[instruction_count]) = const_cast<void*>(ptr);
    instruction_count += sizeof(void*) / sizeof(uint32_t);
}

void Trampoline::WriteData(void const* ptr, uint32_t size) {
    // TODO: Write this to a different buffer and return a pointer to it
    FLAMINGO_ASSERT((size + instruction_count) * sizeof(uint32_t) <= alloc_size);
    FLAMINGO_DEBUG("Writing data from: {} of size: {}", ptr, size * sizeof(uint32_t));
    std::memcpy(&address[instruction_count], ptr, size * sizeof(uint32_t));
    instruction_count += size;
}

void Trampoline::WriteB(int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/B--Branch-
    WriteCallback(reinterpret_cast<uint32_t*>(imm));
}

void Trampoline::WriteBl(int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/BL--Branch-with-Link-
    constexpr uint32_t branch_imm_mask = 0b00000011111111111111111111111111U;
    int64_t pc = get_untagged_pc(reinterpret_cast<uint64_t>(&address[instruction_count]));
    int64_t delta = imm - pc;
    if (std::llabs(delta) > (branch_imm_mask << 1) + 1) {
        // Too far to emit a b. Emit a br instead.
        // We cannot emit a blr here because the pc + 4 for return will be in our offset.
        // LDR X17, #12
        constexpr uint32_t ldr_x17 = 0x58000071U;
        Write(ldr_x17);
        // ADR X30, #16
        constexpr uint32_t adr_x30 = 0x1000009EU;
        Write(adr_x30);
        // BR x17
        constexpr uint32_t br_x17 = 0xD61F0220U;
        Write(br_x17);
        // Data
        WriteData(reinterpret_cast<void*>(imm));
    } else {
        // Small enough to emit a b/bl.
        // bl opcode | encoded immediate (delta >> 2)
        // Note, abs(delta >> 2) must be < (1 << 26)
        constexpr uint32_t bl_opcode = 0b10010100000000000000000000000000U;
        Write(bl_opcode | ((delta >> 2) & branch_imm_mask));
    }
}

void Trampoline::WriteAdr(uint8_t reg, int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/ADR--Form-PC-relative-address-?lang=en
    constexpr uint32_t adr_maximum_imm = 0b00000000000111111111111111111111U;
    constexpr uint32_t reg_mask = 0b11111;
    int64_t pc = get_untagged_pc(reinterpret_cast<uint64_t>(&address[instruction_count]));
    int64_t delta = imm - pc;
    if (std::llabs(delta) >= (adr_maximum_imm >> 1)) {
        // Too far to emit just an adr.
        // LDR (used register), #0x8
        constexpr uint32_t ldr_mask = 0b01011000000000000000000000000000U;
        // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
        // imm is encoded as << 2, LSB just to the right of reg
        constexpr uint32_t ldr_imm = (8U >> 2U) << 5;
        Write(ldr_mask | ldr_imm | (reg_mask & reg));
        // B #0xC
        constexpr uint32_t b_0xc = 0x14000003U;
        static_assert(b_0xc == get_b(0xC));
        Write(get_b(0xC));
        // Immediate data
        WriteData(reinterpret_cast<void*>(imm));
    } else {
        // Close enough to emit an adr.
        // Note that delta should be within +-1 MB
        constexpr uint32_t adr_opcode = 0b00010000000000000000000000000000;
        // Get immlo
        uint32_t imm_lo = ((static_cast<uint32_t>(delta) & 3) << 29);
        // Get immhi
        uint32_t imm_hi = (static_cast<uint32_t>(delta) >> 2) << 5;
        Write(adr_opcode | imm_lo | imm_hi | (reg_mask & reg));
    }
}

void Trampoline::WriteAdrp(uint8_t reg, int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/ADR--Form-PC-relative-address-?lang=en
    // constexpr uint32_t adr_maximum_imm = 0b00000000000111111111111111111111U;
    constexpr uint32_t reg_mask = 0b11111;
    constexpr uint32_t pc_imm_mask = ~0b111111111111;
    constexpr int64_t adrp_maximum_imm = 0xFFFFF000U;
    int64_t pc = get_untagged_pc(reinterpret_cast<uint64_t>(&address[instruction_count]));
    int64_t delta = (pc & pc_imm_mask) - imm;
    if (std::llabs(delta) >= adrp_maximum_imm) {
        // Too far to emit just an adr.
        // LDR (used register), #0x8
        constexpr uint32_t ldr_mask = 0b01011000000000000000000000000000U;
        // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
        // imm is encoded as << 2, LSB just to the right of reg
        constexpr uint32_t ldr_imm = (8U >> 2U) << 5;
        Write(ldr_mask | ldr_imm | (reg_mask & reg));
        // B #0xC
        constexpr uint32_t b_0xc = 0x14000003U;
        // TODO: Remove this assertion
        static_assert(b_0xc == get_b(0xC));
        Write(get_b(0xC));
        // Write(b_0xc);
        // Immediate data
        WriteData(reinterpret_cast<void*>(imm));
    } else {
        // Close enough to emit an adrp.
        // Note that delta should be within +-4 GB
        constexpr uint32_t adrp_opcode = 0b10010000000000000000000000000000U;
        // Imm is << 12 in parse of instruction
        delta >>= 12;
        // Get immlo
        uint32_t imm_lo = ((static_cast<uint32_t>(delta) & 3) << 29);
        // Get immhi
        uint32_t imm_hi = (static_cast<uint32_t>(delta) >> 2) << 5;
        Write(adrp_opcode | imm_lo | imm_hi | (reg_mask & reg));
    }
}

void Trampoline::WriteLdr(uint32_t inst, uint8_t reg, int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
    constexpr uint32_t reg_mask = 0b11111;
    // 20 bits because signed range is only allowed
    // constexpr int64_t max_ldr_range = (1LL << 20);
    if ((inst & 0xFF000000U) == 0xD8000000U) {
        // This is a prefetch instruction.
        // Lets just skip it.
        return;
    }
    // int64_t pc = get_untagged_pc(reinterpret_cast<uint64_t>(&address[instruction_count]));
    // int64_t delta = imm - pc;
    // TODO: Note missed optimization opportunity
    // TODO: Should perform a small LDR
    FLAMINGO_DEBUG("Potentially missed optimization opportunity for near LDRs!");

    // if (std::llabs(delta) >= max_ldr_range) {

    // Too far to emit an equivalent LDR
    // Fallback to performing a direct memory write/read
    // LDR (used register), #0x8
    constexpr uint32_t ldr_mask = 0b01011000000000000000000000000000U;
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
    // imm is encoded as << 2, LSB just to the right of reg
    constexpr uint32_t ldr_imm = (8U >> 2U) << 5;
    Write(ldr_mask | ldr_imm | (reg_mask & reg));
    // B #0xC
    Write(get_b(0xC));
    // Immediate data
    // 4, 8, 16(?) for our sizes
    constexpr uint32_t size_mask = 0x40000000U;
    // TODO: Pull the start address from this data write
    WriteData(reinterpret_cast<void*>(imm), 1);
    if ((inst & size_mask) != 0) {
        WriteData(reinterpret_cast<void*>(imm + sizeof(uint32_t)), 1);
    }

    // } else {
    //     // Close enough to emit an LDR
    //     FLAMINGO_ABORT("Close LDR optimization is not implemented (with no fallback)!");
    // }
}

template <bool imm_19>
void WriteCondBranch(Trampoline& value, uint32_t instruction, int64_t imm) {
    uint32_t imm_mask;
    if constexpr (imm_19) {
        // Imm 19
        constexpr uint32_t imm_mask_19 = 0b00000000111111111111111111100000;
        imm_mask = imm_mask_19;
    } else {
        // Imm 14
        constexpr uint32_t imm_mask_14 = 0b00000000000001111111111111100000;
        imm_mask = imm_mask_14;
    }
    int64_t pc = get_untagged_pc(reinterpret_cast<uint64_t>(&value.address[value.instruction_count]));
    int64_t delta = imm - pc;
    // imm_mask >> 1 for maximum positive value
    // << 2 because branch imms are << 2
    // >> 5 because the mask is too high
    if (std::llabs(delta) < (imm_mask >> 4)) {
        // Small enough to optimize, just write the instruction
        // But with the modified offset
        // Delta should be >> 2 for branch imm
        // Then << 5 to be in the correct location
        value.Write((instruction & ~imm_mask) | (((delta >> 2) << 5) & imm_mask));
    } else {
        // Otherwise, we need to write the same expression but with a known offset
        // Specifically, write the instruction but with an offset of 8
        // 2, because 8 >> 2 is 2
        // << 5 to place in correct location for immediate
        value.Write((instruction & ~imm_mask) | ((2 << 5) & imm_mask));
        value.Write(get_b(0x14));
        value.WriteLdrBrData(reinterpret_cast<uint32_t*>(imm));
    }
}

void Trampoline::WriteLdrBrData(uint32_t const* target) {
    // LDR x17, 0x8
    constexpr uint32_t ldr_x17 = 0x58000051U;
    Write(ldr_x17);
    // BR x17
    constexpr uint32_t br_x17 = 0xD61F0220U;
    Write(br_x17);
    // Data
    WriteData(target);
}

auto get_branch_immediate(cs_insn const& inst) {
    FLAMINGO_ASSERT(inst.detail->arm64.op_count == 1);
    return inst.detail->arm64.operands[0].imm;
}

std::pair<uint8_t, int64_t> get_second_immediate(cs_insn const& inst) {
    // register is just bottom 5 bits
    constexpr uint32_t reg_mask = 0b11111;
    FLAMINGO_ASSERT(inst.detail->arm64.op_count == 2);
    return { *reinterpret_cast<uint32_t const*>(inst.bytes) & reg_mask, inst.detail->arm64.operands[1].imm };
}

std::pair<uint8_t, int64_t> get_last_immediate(cs_insn const& inst) {
    // register is just bottom 5 bits
    constexpr uint32_t reg_mask = 0b11111;
    FLAMINGO_ASSERT(inst.detail->arm64.op_count >= 2);
    return { *reinterpret_cast<uint32_t const*>(inst.bytes) & reg_mask, inst.detail->arm64.operands[inst.detail->arm64.op_count - 11].imm };
}

csh getHandle() {
    static csh handle = 0;
    if (!handle) {
        cs_err e1 = cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle);
        cs_option(handle, CS_OPT_DETAIL, 1);
        if (e1) {
            FLAMINGO_ABORT("Capstone initialization failed");
        }
    }
    return handle;
}

cs_insn debugInst(uint32_t const* inst) {
    cs_insn* insns = nullptr;
    auto count = cs_disasm(getHandle(), reinterpret_cast<uint8_t const*>(inst), sizeof(uint32_t), static_cast<uint64_t>(get_untagged_pc(reinterpret_cast<uint64_t>(inst))), 1, &insns);
    if (count == 1) {
        return insns[0];
    }
    return {};
}

void Trampoline::WriteFixup(uint32_t const* target) {
    // TODO: Make this faster for cases where we will write many fixups
    FLAMINGO_ASSERT(target);
    // Target is where we want to grab original instruction from
    // Log everything we do here
    original_instructions.push_back(*target);
    cs_insn* insns = nullptr;
    auto count = cs_disasm(getHandle(), reinterpret_cast<uint8_t const*>(target), sizeof(uint32_t), static_cast<uint64_t>(get_untagged_pc(reinterpret_cast<uint64_t>(target))), 1, &insns);
    FLAMINGO_ASSERT(count == 1);
    auto inst = insns[0];
    // constexpr uint32_t cond_branch_mask = 0b11111111000000000000000000011111;
    // TODO: Finish writing fixups here
    FLAMINGO_DEBUG("Fixup for inst: 0x{:x} at {}: {} {}, id: {}", *target, fmt::ptr(target), fmt::string_view(inst.mnemonic, sizeof(inst.mnemonic)), fmt::string_view(inst.op_str, sizeof(inst.op_str)),
                   static_cast<int>(inst.id));
    switch (inst.id) {
        // Handle fixups for branch immediate
        case ARM64_INS_B: {
            FLAMINGO_DEBUG("Fixing up B...");
            if (inst.detail->arm64.cc != ARM64_CC_INVALID) {
                // TODO: Handle this like a conditional branch
                auto dst = get_branch_immediate(inst);
                WriteCondBranch<true>(*this, *target, dst);
            } else {
                auto dst = get_branch_immediate(inst);
                WriteB(dst);
            }
        } break;
        case ARM64_INS_BL: {
            FLAMINGO_DEBUG("Fixing up BL...");
            auto dst = get_branch_immediate(inst);
            WriteBl(dst);
        } break;

        // Handle fixups for conditional branches
        case ARM64_INS_CBNZ:
        case ARM64_INS_CBZ: {
            FLAMINGO_DEBUG("Fixing up CBNZ/CBZ...");
            auto [reg, dst] = get_last_immediate(inst);
            WriteCondBranch<true>(*this, *target, dst);
        } break;
        case ARM64_INS_TBNZ:
        case ARM64_INS_TBZ: {
            FLAMINGO_DEBUG("Fixing up TBNZ/TBZ...");
            auto [reg, dst] = get_last_immediate(inst);
            WriteCondBranch<false>(*this, *target, dst);
        } break;

        // Handle fixups for load literals
        case ARM64_INS_LDR: {
            FLAMINGO_DEBUG("Fixing up LDR...");
            // TODO: Finish this fixup
            constexpr uint32_t b_31 = 0b10000000000000000000000000000000;
            constexpr uint32_t ldr_lit_opc_mask = 0b10111111000000000000000000000000;
            if ((*target & ldr_lit_opc_mask) == 0b00011000000000000000000000000000) {
                // This is an ldr literal
                // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
                auto [reg, dst] = get_second_immediate(inst);
                WriteLdr(*target, reg, dst);
            } else if ((*target & (ldr_lit_opc_mask & ~b_31)) == 0b00011100000000000000000000000000000) {
                // This is an ldr literal, SIMD
                // https://developer.arm.com/documentation/ddi0596/2021-12/SIMD-FP-Instructions/LDR--literal--SIMD-FP---Load-SIMD-FP-Register--PC-relative-literal--
                FLAMINGO_ABORT("LDR of the SIMD variant is not yet supported!");
            }
        } break;
        case ARM64_INS_LDRSW: {
            // This is an ldrsw literal
            // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDRSW--literal---Load-Register-Signed-Word--literal--
            FLAMINGO_ABORT("LDRSW fixup not yet supported!");
        } break;

        // Handle pc-relative loads
        case ARM64_INS_ADR: {
            FLAMINGO_DEBUG("Fixing up ADR...");
            auto [reg, dst] = get_second_immediate(inst);
            WriteAdr(reg, dst);
        } break;
        case ARM64_INS_ADRP: {
            FLAMINGO_DEBUG("Fixing up ADRP...");
            auto [reg, dst] = get_second_immediate(inst);
            WriteAdrp(reg, dst);
        } break;

        // Otherwise, just write the instruction verbatim
        default:
            FLAMINGO_DEBUG("Fixing up UNKNOWN: {}...", inst.id);
            Write(*reinterpret_cast<uint32_t*>(inst.bytes));
            break;
    }
}

void Trampoline::WriteCallback(uint32_t const* target) {
    constexpr uint32_t branch_imm_mask = 0b00000011111111111111111111111111U;
    int64_t pc = get_untagged_pc(reinterpret_cast<uint64_t>(&address[instruction_count]));
    auto delta = reinterpret_cast<int64_t>(target) - pc;
    if (std::llabs(delta) > (branch_imm_mask << 1) + 1) {
        // Too far for b. Emit a br instead.
        WriteLdrBrData(target);
    } else {
        // Small enough to emit a b.
        // b opcode | encoded immediate (delta >> 2)
        // Note, abs(delta >> 2) must be < (1 << 26)
        constexpr uint32_t b_opcode = 0b00010100000000000000000000000000U;
        Write(b_opcode | ((delta >> 2) & branch_imm_mask));
    }
}

void Trampoline::WriteFixups(uint32_t const* target, uint16_t countToFixup) {
    original_instructions.reserve(countToFixup);
    FLAMINGO_DEBUG("Fixing up: {} instructions!", countToFixup);
    while (countToFixup-- > 0) {
        WriteFixup(target++);
    }
    WriteCallback(target);
    Finish();
}

void Trampoline::Finish() {
    pageSizeRef -= alloc_size - (instruction_count * sizeof(uint32_t));
    FLAMINGO_DEBUG("Completed trampoline allocation of: {} instructions!", instruction_count);
}

void Trampoline::Log() {
    // TODO: Log the trampoline and various information here
    // This will probably be necessary given the potential for failure
}

}  // namespace flamingo
