#include <fmt/core.h>
#include <cstdint>
#include <span>
#include <utility>

#include "calling-convention.hpp"
#include "hook-data.hpp"
#include "hook-metadata.hpp"
#include "installer.hpp"
#include "page-allocator.hpp"
#include "target-data.hpp"
#include "test-wrapper.hpp"

using namespace flamingo;

static std::span<uint32_t> perform_far_hook_test(uintptr_t hook_location, std::span<uint8_t> to_hook) {
  std::span<uint32_t> hook_span = std::span<uint32_t>(reinterpret_cast<uint32_t*>(&to_hook[0]),
                                                      reinterpret_cast<uint32_t*>(&to_hook[to_hook.size()]));
  return alloc_far(flamingo::PointerWrapper<uint32_t>(std::span<uint32_t>(reinterpret_cast<uint32_t*>(hook_location),
                                                                          reinterpret_cast<uint32_t*>(hook_location)),
                                                      flamingo::PageProtectionType::kNone),
                   hook_span);
}

static void test_name_matching() {
  puts("Test: name matching");
  // Setup
  uintptr_t hook_function_A = 0x11110001;
  uintptr_t hook_function_B = 0x22220002;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f,
                            0x02, 0xa9, 0xfd, 0x7b, 0x03, 0xa9, 0xfd, 0xc3, 0x00, 0x91 };

  auto hook_target = perform_far_hook_test(hook_function_A, to_hook);

  void* origA = nullptr;
  void* origB = nullptr;

  // Install B first, but request that B is installed after A (i.e., B.afters = {A})
  HookNameMetadata nameA;
  nameA.name = "A";
  HookNameMetadata nameB;
  nameB.name = "B";
  HookPriority priorityB;
  priorityB.afters.push_back(nameA);

  flamingo::HookInfo hB((void*)hook_function_B, hook_target.data(), &origB, std::move(nameB), std::move(priorityB));
  auto resB = flamingo::Install(std::move(hB));
  if (!resB.has_value()) ERROR("Failed to install B: {}", resB.error());

  // Now install A; final ordering should be A then B
  HookNameMetadata nmA;
  nmA.name = "A";
  HookPriority pA;
  flamingo::HookInfo hA((void*)hook_function_A, hook_target.data(), &origA, std::move(nmA), std::move(pA));
  auto resA = flamingo::Install(std::move(hA));
  if (!resA.has_value()) ERROR("Failed to install A: {}", resA.error());

  // Validate ordering: A should be first (origA == B.hook_ptr), B should be last (origB == fixups)
  auto fixup_res = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target.data()));
  if (!fixup_res.has_value()) {
    ERROR("Failed to get fixup pointer");
  }
  void* fixup_ptr = (void*)fixup_res.value().data();

  if ((uintptr_t)origA != hook_function_B) {
    ERROR("Name-matching: expected A.orig == B.hook_ptr (0x{:x}) but got 0x{:x}", hook_function_B, (uintptr_t)origA);
  }
  if ((uintptr_t)origB != (uintptr_t)fixup_ptr) {
    ERROR("Name-matching: expected B.orig == fixups (ptr {}) but got 0x{:x}", fmt::ptr(fixup_ptr), (uintptr_t)origB);
  }
}

static void test_namespaze_matching() {
  puts("Test: namespaze matching");
  // Setup three hooks: two in the same namespaze and one that must come before that namespaze
  uintptr_t hf1 = 0x33330001;
  uintptr_t hf2 = 0x33330002;
  uintptr_t prior = 0x44440004;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f };

  auto hook_target = perform_far_hook_test(hf1, to_hook);

  void* orig1 = nullptr;
  void* orig2 = nullptr;
  void* orig_prior = nullptr;

  HookNameMetadata n1;
  n1.name = "one";
  n1.namespaze = "common";
  HookNameMetadata n2;
  n2.name = "two";
  n2.namespaze = "common";

  // Install first two in the same namespaze
  flamingo::HookInfo h1((void*)hf1, hook_target.data(), &orig1, std::move(n1), HookPriority{});
  auto r1 = flamingo::Install(std::move(h1));
  if (!r1.has_value()) ERROR("Failed to install h1: {}", r1.error());

  HookNameMetadata tmp2;
  tmp2.name = "two";
  tmp2.namespaze = "common";
  flamingo::HookInfo h2((void*)hf2, hook_target.data(), &orig2, std::move(tmp2), HookPriority{});
  auto r2 = flamingo::Install(std::move(h2));
  if (!r2.has_value()) ERROR("Failed to install h2: {}", r2.error());

  // Now install prior that requests to be before the entire namespaze "common"
  HookNameMetadata prior_name;
  prior_name.name = "prior";
  HookPriority prior_prio;
  HookNameMetadata match_ns;
  match_ns.namespaze = "common";
  prior_prio.befores.push_back(match_ns);
  flamingo::HookInfo hprior((void*)prior, hook_target.data(), &orig_prior, std::move(prior_name),
                            std::move(prior_prio));
  auto rp = flamingo::Install(std::move(hprior));
  if (!rp.has_value()) ERROR("Failed to install prior: {}", rp.error());

  // Validate ordering: prior -> hf1 -> hf2 (hf1 and hf2 preserve relative order)
  auto fixup_res = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target.data()));
  if (!fixup_res.has_value()) ERROR("Failed to get fixup pointer");
  void* fixup_ptr = (void*)fixup_res.value().data();

  if ((uintptr_t)orig_prior != hf1) {
    ERROR("Namespaze-matching: expected prior.orig == hf1 (0x{:x}) but got 0x{:x}", hf1, (uintptr_t)orig_prior);
  }
  if ((uintptr_t)orig1 != hf2) {
    ERROR("Namespaze-matching: expected hf1.orig == hf2 (0x{:x}) but got 0x{:x}", hf2, (uintptr_t)orig1);
  }
  if ((uintptr_t)orig2 != (uintptr_t)fixup_ptr) {
    ERROR("Namespaze-matching: expected hf2.orig == fixup but got 0x{:x}", (uintptr_t)orig2);
  }
}

int main() {
  test_name_matching();
  test_namespaze_matching();
  puts("SORTED HOOKS TESTS PASSED");
}
