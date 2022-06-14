#include "logger.hpp"
#include "trampoline-allocator.hpp"
#include <list>
#include <sys/mman.h>
#include <cstdlib>
#include "beatsaber-hook/shared/utils/utils-functions.h"

#ifdef ID
#define __ID_BKP ID
#endif
#define ID MOD_ID
#ifdef VERSION
#define __VERSION_BKP VERSION
#endif
#define VERSION MOD_VERSION
#include "beatsaber-hook/shared/utils/capstone-utils.hpp"
#undef ID
#ifdef __ID_BKP
#define ID __ID_BKP
#endif
#undef VERSION
#ifdef __VERSION_BKP
#define VERSION __VERSION_BKP
#endif

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

void Trampoline::Write(uint32_t instruction) {
    assert((instruction_count + 1) * sizeof(uint32_t) <= alloc_size);
    flamingo::Logger.fmtLog<Paper::LogLevel::DBG>("Trampoline writing instruction {} count {} instruction {}", fmt::ptr(address), instruction_count, instruction);
    // Log what we are writing (and also our state)
    *(address + instruction_count) = instruction;
    instruction_count++;
}

void Trampoline::Write(void const* ptr) {
    assert(instruction_count * sizeof(uint32_t) + sizeof(void*) <= alloc_size);
    // Log what we are writing (and also our state)
    flamingo::Logger.fmtLog<Paper::LogLevel::DBG>("Trampoline writing pointer instruction {} count {} ptr {}", fmt::ptr(address), instruction_count, fmt::ptr(ptr));

    *reinterpret_cast<void**>(address + instruction_count) = const_cast<void*>(ptr);
    instruction_count += sizeof(void*) / sizeof(uint32_t);
}

void Trampoline::WriteB(int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/B--Branch-
    WriteCallback(reinterpret_cast<uint32_t*>(imm));
}

void Trampoline::WriteBl(int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/BL--Branch-with-Link-
    constexpr static uint32_t branch_imm_mask = 0b00000011111111111111111111111111U;
    int64_t pc = reinterpret_cast<int64_t>(address + instruction_count);
    int64_t delta = pc - imm;
    if (std::llabs(delta) > (branch_imm_mask << 1) + 1) {
        // Too far to emit a b. Emit a br instead.
        // We cannot emit a blr here because the pc + 4 for return will be in our offset.
        // LDR X17, #12
        constexpr static uint32_t ldr_x17 = 0x58000071U;
        Write(ldr_x17);
        // ADR X30, #16
        constexpr static uint32_t adr_x30 = 0x1000009EU;
        Write(adr_x30);
        // BR x17
        constexpr static uint32_t br_x17 = 0xD61F0220U;
        Write(br_x17);
        // Data
        Write(reinterpret_cast<void*>(imm));
    } else {
        // Small enough to emit a b/bl.
        // bl opcode | encoded immediate (delta >> 2)
        // Note, abs(delta >> 2) must be < (1 << 26)
        constexpr static uint32_t bl_opcode = 0b10010100000000000000000000000000U;
        Write(bl_opcode | ((delta >> 2) & branch_imm_mask));
    }
}

void Trampoline::WriteAdr(uint8_t reg, int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/ADR--Form-PC-relative-address-?lang=en
    constexpr static uint32_t adr_maximum_imm = 0b00000000000111111111111111111111U;
    constexpr static uint32_t reg_mask = 0b11111;
    int64_t pc = reinterpret_cast<int64_t>(address + instruction_count);
    int64_t delta = pc - imm;
    if (std::llabs(delta) >= (adr_maximum_imm >> 1)) {
        // Too far to emit just an adr.
        // LDR (used register), #0x8
        constexpr static uint32_t ldr_mask = 0b01011000000000000000000000000000U;
        // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
        // imm is encoded as << 2, LSB just to the right of reg
        constexpr static uint32_t ldr_imm = (8U >> 2U) << 5;
        Write(ldr_mask | ldr_imm | (reg_mask & reg));
        // B #0xC
        constexpr static uint32_t b_0xc = 0x14000003U;
        static_assert(b_0xc == get_b(0xC));
        Write(get_b(0xC));
        // Immediate data
        Write(reinterpret_cast<void*>(imm));
    } else {
        // Close enough to emit an adr.
        // Note that delta should be within +-1 MB
        constexpr static uint32_t adr_opcode = 0b00010000000000000000000000000000;
        // Get immlo
        uint32_t imm_lo = ((static_cast<uint32_t>(delta) & 3) << 29);
        // Get immhi
        uint32_t imm_hi = (static_cast<uint32_t>(delta) >> 2) << 5;
        Write(adr_opcode | imm_lo | imm_hi | (reg_mask & reg));
    }
}

void Trampoline::WriteAdrp(uint8_t reg, int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/ADR--Form-PC-relative-address-?lang=en
    // constexpr static uint32_t adr_maximum_imm = 0b00000000000111111111111111111111U;
    constexpr static uint32_t reg_mask = 0b11111;
    constexpr static uint32_t pc_imm_mask = ~0b111111111111;
    constexpr static int64_t adrp_maximum_imm = 0xFFFFF000U;
    int64_t pc = reinterpret_cast<int64_t>(address + instruction_count);
    int64_t delta = (pc & pc_imm_mask) - imm;
    if (std::llabs(delta) >= adrp_maximum_imm) {
        // Too far to emit just an adr.
        // LDR (used register), #0x8
        constexpr static uint32_t ldr_mask = 0b01011000000000000000000000000000U;
        // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
        // imm is encoded as << 2, LSB just to the right of reg
        constexpr static uint32_t ldr_imm = (8U >> 2U) << 5;
        Write(ldr_mask | ldr_imm | (reg_mask & reg));
        // B #0xC
        constexpr static uint32_t b_0xc = 0x14000003U;
        // TODO: Remove this assertion
        static_assert(b_0xc == get_b(0xC));
        Write(get_b(0xC));
        // Write(b_0xc);
        // Immediate data
        Write(reinterpret_cast<void*>(imm));
    } else {
        // Close enough to emit an adrp.
        // Note that delta should be within +-4 GB
        constexpr static uint32_t adrp_opcode = 0b10010000000000000000000000000000;
        // Imm is << 12 in parse of instruction
        delta >>= 12;
        // Get immlo
        uint32_t imm_lo = ((static_cast<uint32_t>(delta) & 3) << 29);
        // Get immhi
        uint32_t imm_hi = (static_cast<uint32_t>(delta) >> 2) << 5;
        Write(adrp_opcode | imm_lo | imm_hi | (reg_mask & reg));
    }
}

template<bool imm_19>
void WriteCondBranch(Trampoline& value, uint32_t instruction, int64_t imm) {
    uint32_t imm_mask;
    if constexpr (imm_19) {
        // Imm 19
        constexpr static uint32_t imm_mask_19 = 0b00000000111111111111111111100000;
        imm_mask = imm_mask_19;
    } else {
        // Imm 14
        constexpr static uint32_t imm_mask_14 = 0b00000000000001111111111111100000;
        imm_mask = imm_mask_14;
    }
    int64_t pc = reinterpret_cast<int64_t>(value.address + value.instruction_count);
    int64_t delta = pc - imm;
    // imm_mask >> 1 for maximum positive value
    // << 2 because branch imms are << 2
    // >> 5 because the mask is too high
    if (llabs(delta) < (imm_mask >> 4)) {
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
        value.Write((instruction & ~imm_mask) | (2 << 5) & imm_mask);
        value.Write(get_b(0x14));
        value.WriteLdrBrData(reinterpret_cast<uint32_t*>(imm));
    }
}

void Trampoline::WriteLdrBrData(uint32_t const* target) {
    // LDR x17, 0x8
    constexpr static uint32_t ldr_x17 = 0x58000051U;
    Write(ldr_x17);
    // BR x17
    constexpr static uint32_t br_x17 = 0xD61F0220U;
    Write(br_x17);
    // Data
    Write(target);
}

auto get_branch_immediate(cs_insn const& inst) {
    assert(inst.detail->arm64.op_count == 1);
    return inst.detail->arm64.operands[0].imm;
}

std::pair<uint8_t, int64_t> get_second_immediate(cs_insn const& inst) {
    // register is just bottom 5 bits
    constexpr static uint32_t reg_mask = 0b11111;
    assert(inst.detail->arm64.op_count == 2);
    return {*reinterpret_cast<uint32_t const*>(inst.bytes) & reg_mask, inst.detail->arm64.operands[1].imm};
}

void Trampoline::WriteFixup(uint32_t const* target) {
    // Target is where we want to grab original instruction from
    // Log everything we do here
    original_instructions.push_back(*target);
    cs_insn* insns = nullptr;
    auto count = cs_disasm(cs::getHandle(), reinterpret_cast<uint8_t const*>(target), sizeof(uint32_t), reinterpret_cast<uint64_t>(target), 1, &insns);
    assert(count == 1);
    auto inst = insns[0];
    // constexpr uint32_t cond_branch_mask = 0b11111111000000000000000000011111;
    // TODO: Finish writing fixups here
    switch (inst.id) {
        // Handle fixups for branch immediate
        case ARM64_INS_B:
        {
            if (inst.detail->arm64.cc != ARM64_CC_INVALID) {
                // TODO: Handle this like a conditional branch
                auto dst = get_branch_immediate(inst);
                WriteCondBranch<true>(*this, *target, dst);
            } else {
                auto dst = get_branch_immediate(inst);
                WriteB(dst);
            }
        }
        break;
        case ARM64_INS_BL:
        {
            auto dst = get_branch_immediate(inst);
            WriteBl(dst);
        }
        break;

        // Handle fixups for conditional branches
        case ARM64_INS_CBNZ:
        case ARM64_INS_CBZ:
        {
            auto [reg, dst] = get_second_immediate(inst);
            WriteCondBranch<true>(*this, *target, dst);
        }
        break;
        case ARM64_INS_TBNZ:
        case ARM64_INS_TBZ:
        {
            auto [reg, dst] = get_second_immediate(inst);
            WriteCondBranch<false>(*this, *target, dst);
        }
        break;

        // Handle fixups for load literals
        case ARM64_INS_LDR:
        {
            // TODO: Finish this fixup
            constexpr static uint32_t b_31 = 0b10000000000000000000000000000000;
            constexpr static uint32_t ldr_lit_opc_mask = 0b10111111000000000000000000000000;
            if ((*target & ldr_lit_opc_mask) == 0b00011000000000000000000000000000) {
                // This is an ldr literal
                // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
                auto [reg, dst] = get_second_immediate(inst);
            } else if ((*target & (ldr_lit_opc_mask & ~b_31)) == 0b00011100000000000000000000000000000) {
                // This is an ldr literal, SIMD
                // https://developer.arm.com/documentation/ddi0596/2021-12/SIMD-FP-Instructions/LDR--literal--SIMD-FP---Load-SIMD-FP-Register--PC-relative-literal--
                auto [reg, dst] = get_second_immediate(inst);
                
            }
        }
        case ARM64_INS_LDRSW:
        // TODO: Handle this fixup

        // Handle pc-relative loads
        case ARM64_INS_ADR:
        {
            auto [reg, dst] = get_second_immediate(inst);
            WriteAdr(reg, dst);
        }
        break;
        case ARM64_INS_ADRP:
        {
            auto [reg, dst] = get_second_immediate(inst);
            WriteAdrp(reg, dst);
        }
        break;

        // Otherwise, just write the instruction verbatim
        default:
        Write(*reinterpret_cast<uint32_t*>(inst.bytes));
        break;
    }

}

void Trampoline::WriteCallback(uint32_t const* target) {
    constexpr static uint32_t branch_imm_mask = 0b00000011111111111111111111111111U;
    auto pc = reinterpret_cast<int64_t>(address + instruction_count);
    auto delta = pc - reinterpret_cast<int64_t>(target);
    if (std::llabs(delta) > (branch_imm_mask << 1) + 1) {
        // To far for b. Emit a br instead.
        WriteLdrBrData(target);
    } else {
        // Small enough to emit a b/bl.
        // b opcode | encoded immediate (delta >> 2)
        // Note, abs(delta >> 2) must be < (1 << 26)
        constexpr static uint32_t b_opcode = 0b00010100000000000000000000000000U;
        Write(b_opcode | ((delta >> 2) & branch_imm_mask));
    }
}

void Trampoline::WriteFixups(uint32_t const* target, uint16_t countToFixup) {
    original_instructions.reserve(countToFixup);
    while (countToFixup-- > 0) {
        WriteFixup(target++);
    }
    WriteCallback(target);
    Finish();
}

void Trampoline::Finish() {
    pageSizeRef -= alloc_size - (instruction_count * sizeof(uint32_t));
}

void Trampoline::Log() {
    // TODO: Log the trampoline and various information here
    // This will probably be necessary given the potential for failure

}

struct PageType {
    void* ptr;
    std::size_t used_size;
    uint16_t trampoline_count;

    constexpr PageType(void* p, std::size_t used) : ptr(p), used_size(used), trampoline_count(1) {}
};

std::list<PageType> pages;
constexpr static std::size_t PageSize = 4096;

Trampoline TrampolineAllocator::Allocate(std::size_t trampolineSize) {
    // Allocation should work by grabbing a full page at a time
    // Then we mark the page as rwx
    // Then we should be allowed to use anything on that page until we would need to make another
    // (due to new trampoline being too big to fit)
    // Repeat.
    for (auto& page : pages) {
        // If we have enough space in our page for this trampoline, squeeze it in!
        if (PageSize - page.used_size > trampolineSize) {
            // TODO: We have to be aligned 16 here
            Trampoline to_ret(reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(page.ptr) + page.used_size), trampolineSize, page.used_size);
            page.used_size += trampolineSize;
            page.trampoline_count++;
            // Log allocated trampoline here
            return to_ret;
        }
    }
    // No pages with enough space available.
    void* ptr;
    if (!::posix_memalign(&ptr, PageSize, PageSize)) {
        // Log error on memalign allocation!
        flamingo::Logger.fmtLog<Paper::LogLevel::INF>("Failed to allocate trampoline page of size: {} for size: {}", PageSize, trampolineSize);
        SAFE_ABORT_MSG("Failed to allocate trampoline page of size: %zu for size: %zu", PageSize, trampolineSize);
    }
    // Mark full page as rxw
    if (!::mprotect(ptr, PageSize, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        // Log error on mprotect!
        flamingo::Logger.fmtLog<Paper::LogLevel::INF>("Failed to mark allocated page at: {} as +rwx!", fmt::ptr(ptr));
        SAFE_ABORT_MSG("Failed to mark allocated page at: %p as +rwx!", ptr);
    }
    auto& page = pages.emplace_back(ptr, trampolineSize);
    return {static_cast<uint32_t*>(ptr), trampolineSize, page.used_size};
}

void TrampolineAllocator::Free(Trampoline const& toFree) {
    // Freeing a trampoline should decrease the page it was allocated on's size by a known amount
    // If we reach a point where a trampoline was deallocated on a page and it was the last one in that page, then we should
    // 1. Mark the page as read/write only
    // 2. deallocate the page

    // Find page we are allocated on
    auto page_addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(toFree.address) & ~(PageSize - 1));
    for (auto& p : pages) {
        if (p.ptr == page_addr) {
            p.trampoline_count--;
            if (p.trampoline_count == 0) {
                if (::mprotect(p.ptr, PageSize, PROT_READ) != 0) {
                    // Log error on mprotect
                    flamingo::Logger.fmtLog<Paper::LogLevel::INF>("Failed to mark page at: {} as read only!", fmt::ptr(p.ptr));
                    SAFE_ABORT_MSG("Failed to mark page at: %p as read only!", p.ptr);
                }
                ::free(p.ptr);
            }
            return;
        }
    }
    // If we get here, we couldn't free the provided Trampoline!
    SAFE_ABORT_MSG("Failed to free trampoline at: %p, no matching page with page addr: %p!", toFree.address, page_addr);
}