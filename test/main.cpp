// Main test runner for testing fixups behave as intended
#include <sys/mman.h>
#include <array>
#include <cstdint>
#include <cstdio>

#include "../shared/trampoline-allocator.hpp"
#include "../shared/trampoline.hpp"
#include "capstone/capstone.h"

decltype(auto) test_near(uint32_t* target, uint32_t const* callback) {
    constexpr size_t hookSize = 32;
    constexpr size_t pageSize = 4096;
    constexpr size_t trampolineSize = 64;
    static std::array<uint32_t, trampolineSize> test_trampoline;
    size_t page_size = pageSize;
    printf("TRAMPOLINE: %p\n", test_trampoline.data());
    flamingo::Trampoline trampoline(test_trampoline.data(), sizeof(test_trampoline), page_size);
    // Attempt to write a hook from target --> callback (just for testing purposes)
    std::size_t trampoline_size = hookSize;
    // Hook size is 5, but we only fixup 4
    trampoline.WriteHookFixups(target);
    // Write the jump back to instruction 5
    trampoline.WriteCallback(&target[5]);
    trampoline.Finish();
    // Write actual hook to be a callback
    // We need to mark the location of target as writable (so we can write to it correctly)
    ::mprotect(target, hookSize, PROT_READ | PROT_WRITE | PROT_EXEC);
    ::flamingo::Trampoline targetHook(target, hookSize, trampoline_size);
    targetHook.WriteCallback(callback);
    targetHook.Finish();
    return &test_trampoline;
}

void print_decode_loop(uint32_t* val, int n) {
    auto handle = flamingo::getHandle();
    for (int i = 0; i < n; i++) {
        cs_insn* insns = nullptr;
        auto count = cs_disasm(handle, reinterpret_cast<uint8_t const*>(val), sizeof(uint32_t),
                               static_cast<uint64_t>(reinterpret_cast<uint64_t>(val)), 1, &insns);
        if (count == 1) {
            printf("Addr: %p Value: 0x%x, %s %s\n", val, *val, insns[0].mnemonic, insns[0].op_str);
        } else {
            printf("Addr: %p Value: 0x%x\n", val, *val);
        }
        val++;
    }
}

void perform_near_hook_test(uint8_t* to_hook) {
    printf("TO HOOK: %p\n", to_hook);
    print_decode_loop(reinterpret_cast<uint32_t*>(to_hook), 6);
    puts("TEST NEAR...");
    auto* trampoline_data = test_near(reinterpret_cast<uint32_t*>(to_hook), (const uint32_t*)(0xDEADBEEFBAADF00DULL));
    // Use 20 here as a reasonable guesstimate
    print_decode_loop(trampoline_data->data(), 20);
    puts("HOOKED:");
    print_decode_loop(reinterpret_cast<uint32_t*>(to_hook), 6);
}

void test_near_no_fixups() {
    puts("Testing near -- no fixups!");
    static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x7b, 0x03,
                              0xa9, 0xfd, 0xc3, 0x00, 0x91, 0x48, 0x18, 0x40, 0xf9, 0x16, 0xd4, 0x42, 0xa9, 0xf3, 0x03,
                              0x02, 0xaa, 0xf4, 0x03, 0x01, 0xaa, 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39 };
    perform_near_hook_test(to_hook);
}

void test_near_bls_tbzs_within_hook() {
    puts("Testing near -- bls/tbzs");
    static uint8_t to_hook[]{ 0x68, 0x00, 0x00, 0x37, 0xe0, 0x03, 0x17, 0xaa, 0x52, 0x3e, 0xfd, 0x97,
                              0xe0, 0x03, 0x17, 0xaa, 0x64, 0x7b, 0xfe, 0x97, 0x00, 0x00, 0x00, 0x00 };
    perform_near_hook_test(to_hook);
}

void test_ldr_ldrb_tbnz_bl() {
    puts("Testing near -- ldr/ldrb/tbnz/bl");
    static uint8_t to_hook[]{ 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39, 0x68, 0x00, 0x00, 0x37,
                              0xe0, 0x03, 0x17, 0xaa, 0x52, 0x3e, 0xfd, 0x97, 0x00, 0x00, 0x00, 0x00 };
    perform_near_hook_test(to_hook);
}

// TODO: Test a case where we have a loop in the first 4 instructions

int main() {
    test_near_no_fixups();
    test_near_bls_tbzs_within_hook();
    test_ldr_ldrb_tbnz_bl();
}
