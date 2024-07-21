// Main test runner for testing fixups behave as intended
#include <string.h>
#include <sys/mman.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "../shared/fixups.hpp"
#include "../shared/page-allocator.hpp"
#include "capstone/capstone.h"


void print_decode_loop(std::span<uint32_t> data) {
  auto handle = flamingo::getHandle();
  for (size_t i = 0; i < data.size(); i++) {
    cs_insn* insns = nullptr;
    auto count = cs_disasm(handle, reinterpret_cast<uint8_t const*>(&data[i]), sizeof(uint32_t),
                           reinterpret_cast<uint64_t>(&data[i]), 1, &insns);
    if (count == 1) {
      printf("Addr: %p Value: 0x%08x, %s %s\n", &data[i], data[i], insns[0].mnemonic, insns[0].op_str);
    } else {
      printf("Addr: %p Value: 0x%08x\n", &data[i], data[i]);
    }
  }
}

decltype(auto) test_near(uint32_t* target, [[maybe_unused]] uint32_t const* callback) {
  constexpr size_t hookSizeNumInsts = 5;
  constexpr size_t trampolineSize = 64;
  static std::array<uint32_t, trampolineSize> test_trampoline;
  printf("TRAMPOLINE: %p\n", test_trampoline.data());
  flamingo::Fixups fixups{
    .target = flamingo::PointerWrapper<uint32_t>(std::span<uint32_t>{ target, &target[hookSizeNumInsts - 1] },
                                                 flamingo::PageProtectionType::kExecute |
                                                     flamingo::PageProtectionType::kRead |
                                                     flamingo::PageProtectionType::kWrite),
    .fixup_inst_destination = flamingo::PointerWrapper<uint32_t>(
        test_trampoline, flamingo::PageProtectionType::kExecute | flamingo::PageProtectionType::kRead |
                             flamingo::PageProtectionType::kWrite),
  };
  fixups.PerformFixupsAndCallback();
  return fixups;
}

void perform_near_hook_test(std::span<uint8_t> to_hook) {
  std::span<uint32_t> hook_span = std::span<uint32_t>(reinterpret_cast<uint32_t*>(&to_hook[0]),
                                                      reinterpret_cast<uint32_t*>(&to_hook[to_hook.size()]));
  printf("TO HOOK: %p\n", to_hook.data());
  print_decode_loop(hook_span);
  puts("TEST NEAR...");
  auto trampoline_data = test_near(hook_span.data(), (uint32_t const*)(0xDEADBEEFBAADF00DULL));
  // Use 20 here as a reasonable guesstimate
  print_decode_loop(trampoline_data.fixup_inst_destination.addr);
  puts("HOOKED:");
  print_decode_loop(hook_span);
}

decltype(auto) test_far(uint32_t* target, [[maybe_unused]] uint32_t const* callback) {
  constexpr size_t hookSizeNumInsts = 5;
  constexpr size_t trampolineSize = 64;
  // We allocate the page with r-x perms, we will mark it as writable when we do the writes and otherwise put it back to
  // this state.
  auto fixup_ptr = flamingo::Allocate(16, trampolineSize * sizeof(uint32_t),
                                      flamingo::PageProtectionType::kExecute | flamingo::PageProtectionType::kRead);
  flamingo::Fixups fixups{
    .target = flamingo::PointerWrapper<uint32_t>(
        std::span<uint32_t>{ target, &target[hookSizeNumInsts - 1] },
        flamingo::PageProtectionType::kExecute | flamingo::PageProtectionType::kRead),
    .fixup_inst_destination = fixup_ptr,
  };
  printf("TRAMPOLINE: %p\n", &fixup_ptr.addr[0]);
  // Attempt to write a hook from target --> callback (just for testing purposes)
  // Hook size is 5, but we only fixup 4
  fixups.PerformFixupsAndCallback();
  return fixups;
}

void perform_far_hook_test(std::span<uint8_t> to_hook) {
  std::span<uint32_t> hook_span = std::span<uint32_t>(reinterpret_cast<uint32_t*>(&to_hook[0]),
                                                      reinterpret_cast<uint32_t*>(&to_hook[to_hook.size()]));
  printf("TO HOOK: %p\n", to_hook.data());
  print_decode_loop(hook_span);
  puts("TEST FAR...");
  auto trampoline = test_far(hook_span.data(), (uint32_t const*)(0xDEADBEEFBAADF00DULL));
  // Use 20 here as a reasonable guesstimate
  print_decode_loop(trampoline.fixup_inst_destination.addr);
  puts("HOOKED:");
  print_decode_loop(hook_span);
}

void test_no_fixups() {
  puts("Testing near -- no fixups!");
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x7b, 0x03,
                            0xa9, 0xfd, 0xc3, 0x00, 0x91, 0x48, 0x18, 0x40, 0xf9, 0x16, 0xd4, 0x42, 0xa9, 0xf3, 0x03,
                            0x02, 0xaa, 0xf4, 0x03, 0x01, 0xaa, 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39 };
  perform_near_hook_test(to_hook);
  puts("Testing far -- no fixups!");
  perform_far_hook_test(to_hook);
}

void test_bls_tbzs_within_hook() {
  puts("Testing near -- bls/tbzs");
  static uint8_t to_hook[]{ 0x68, 0x00, 0x00, 0x37, 0xe0, 0x03, 0x17, 0xaa, 0x52, 0x3e, 0xfd, 0x97,
                            0xe0, 0x03, 0x17, 0xaa, 0x64, 0x7b, 0xfe, 0x97, 0x00, 0x00, 0x00, 0x00 };
  perform_near_hook_test(to_hook);
  puts("Testing far -- bls/tbzs");
  perform_far_hook_test(to_hook);
}

void test_ldr_ldrb_tbnz_bl() {
  puts("Testing near -- ldr/ldrb/tbnz/bl");
  static uint8_t to_hook[]{ 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39, 0x68, 0x00, 0x00, 0x37,
                            0xe0, 0x03, 0x17, 0xaa, 0x52, 0x3e, 0xfd, 0x97, 0x00, 0x00, 0x00, 0x00 };
  perform_near_hook_test(to_hook);
  puts("Testing far -- ldr/ldrb/tbnz/bl");
  perform_far_hook_test(to_hook);
}

void test_adrp() {
  puts("Testing near -- adrp");
  static uint8_t to_hook[]{ 0x09, 0x00, 0x00, 0x90, 0xa8, 0x00, 0x80, 0x52, 0x28, 0x01,
                            0x00, 0xb9, 0x28, 0x01, 0x00, 0xb9, 0xc0, 0x03, 0x5f, 0xd6 };
  perform_near_hook_test(to_hook);
  puts("Testing far -- adrp");
  perform_far_hook_test(to_hook);
}

// TODO: Test a case where we have a loop in the first 4 instructions
// TODO: Test a case where we have an ldr literal that loads from within fixup range

int main() {
  test_no_fixups();
  test_bls_tbzs_within_hook();
  test_ldr_ldrb_tbnz_bl();
  test_adrp();
}
