#include <fmt/core.h>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <ostream>
#include <span>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <string>

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

 fmt::println("Installed hooks hf1=0x{:x}, hf2=0x{:x}\n", hf1, hf2); 
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

  // Newer installs are placed at the front, so when h1 then h2 were installed
  // the preserved order is (two -> one). Thus after inserting `prior` before
  // the `common` namespace, expected chain is: prior -> two -> one.
  if ((uintptr_t)orig_prior != hf2) {
    ERROR("Namespaze-matching: expected prior.orig == hf2 (0x{:x}) but got 0x{:x}", hf2, (uintptr_t)orig_prior);
  }
  if ((uintptr_t)orig2 != hf1) {
    ERROR("Namespaze-matching: expected hf2.orig == hf1 (0x{:x}) but got 0x{:x}", hf1, (uintptr_t)orig2);
  }
  if ((uintptr_t)orig1 != (uintptr_t)fixup_ptr) {
    ERROR("Namespaze-matching: expected hf1.orig == fixup but got 0x{:x}", (uintptr_t)orig1);
  }
}



static void test_priority_cycle() {
  puts("Test: priority cycle");
  uintptr_t hx = 0xaaaa0001;
  uintptr_t hy = 0xbbbb0002;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9 };

  auto hook_target = perform_far_hook_test(hx, to_hook);
  void* origX = nullptr;
  void* origY = nullptr;

  HookNameMetadata nX; nX.name = "X";
  HookNameMetadata nY; nY.name = "Y";
  HookPriority pX; pX.afters.push_back(nY);
  HookPriority pY; pY.afters.push_back(nX);

  flamingo::HookInfo hX((void*)hx, hook_target.data(), &origX, std::move(nX), std::move(pX));
  auto rX = flamingo::Install(std::move(hX));
  if (!rX.has_value()) ERROR("Failed to install X: {}", rX.error());

  // installing this should trigger a cycle and return an error; the original
  // state from after installing X should be retained.
  flamingo::HookInfo hY((void*)hy, hook_target.data(), &origY, std::move(nY), std::move(pY));
  auto rY = flamingo::Install(std::move(hY));
  if (rY.has_value()) ERROR("Expected Y install to fail due to cycle, but it succeeded");

  auto fixup_res = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target.data()));
  if (!fixup_res.has_value()) ERROR("Failed to get fixup pointer");
  void* fixup_ptr = (void*)fixup_res.value().data();

  // Since Y failed to install, the original state from after installing X
  // must be preserved: X.orig should still point to the fixup pointer, and
  // Y should not have been installed (origY stays null).
  if ((uintptr_t)origX != (uintptr_t)fixup_ptr) {
    ERROR("Priority-cycle: expected X.orig to remain pointing at fixups (0x{:x}) but got 0x{:x}",
          (uintptr_t)fixup_ptr, (uintptr_t)origX);
  }
  if (origY != nullptr) {
    ERROR("Priority-cycle: expected Y not to be installed (origY == nullptr) but got 0x{:x}", (uintptr_t)origY);
  }
}

static void test_complex_namespace() {
  puts("Test: complex namespace ordering");
  uintptr_t a1 = 0x10010001;
  uintptr_t a2 = 0x10010002;
  uintptr_t b1 = 0x20020001;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57 };

  auto hook_target = perform_far_hook_test(a1, to_hook);
  void* origA1 = nullptr; void* origA2 = nullptr; void* origB1 = nullptr;

  HookNameMetadata ma1; ma1.name = "a1"; ma1.namespaze = "alpha";
  HookNameMetadata ma2; ma2.name = "a2"; ma2.namespaze = "alpha";
  HookNameMetadata mb1; mb1.name = "b1"; mb1.namespaze = "beta";

  flamingo::HookInfo hA1((void*)a1, hook_target.data(), &origA1, std::move(ma1), HookPriority{});
  if (!flamingo::Install(std::move(hA1)).has_value()) ERROR("Failed to install a1");

  flamingo::HookInfo hA2((void*)a2, hook_target.data(), &origA2, std::move(ma2), HookPriority{});
  if (!flamingo::Install(std::move(hA2)).has_value()) ERROR("Failed to install a2");

  // b1 requests to be before the entire namespaze "alpha"
  HookNameMetadata match_ns; match_ns.namespaze = "alpha";
  HookPriority pB; pB.befores.push_back(match_ns);
  flamingo::HookInfo hB1((void*)b1, hook_target.data(), &origB1, std::move(mb1), std::move(pB));
  if (!flamingo::Install(std::move(hB1)).has_value()) ERROR("Failed to install b1");

  auto fixup_res = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target.data()));
  if (!fixup_res.has_value()) ERROR("Failed to get fixup pointer");
  void* fixup_ptr = (void*)fixup_res.value().data();

  // Newer installs are at the front; after installing a1 then a2 the preserved
  // order is a2 -> a1. Placing b1 before the 'alpha' namespace yields:
  // Expect b1 -> a2 -> a1
  if ((uintptr_t)origB1 != a2) ERROR("Complex-ns: expected b1.orig == a2 (0x{:x}) got 0x{:x}", a2, (uintptr_t)origB1);
  if ((uintptr_t)origA2 != a1) ERROR("Complex-ns: expected a2.orig == a1 (0x{:x}) got 0x{:x}", a1, (uintptr_t)origA2);
  if ((uintptr_t)origA1 != (uintptr_t)fixup_ptr) ERROR("Complex-ns: expected a1.orig == fixup got 0x{:x}", (uintptr_t)origA1);
}

static void test_final_conflict() {
  puts("Test: final hook conflict");
  uintptr_t f1 = 0x90010001;
  uintptr_t f2 = 0x90020002;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8 };

  auto hook_target = perform_far_hook_test(f1, to_hook);
  void* origF1 = nullptr; void* origF2 = nullptr;

  HookNameMetadata n1; n1.name = "final1";
  HookPriority p1; p1.is_final = true;
  flamingo::HookInfo hF1((void*)f1, hook_target.data(), &origF1, std::move(n1), std::move(p1));
  auto r1 = flamingo::Install(std::move(hF1));
  if (!r1.has_value()) ERROR("Failed to install final1: {}", r1.error());

  HookNameMetadata n2; n2.name = "final2";
  HookPriority p2; p2.is_final = true;
  flamingo::HookInfo hF2((void*)f2, hook_target.data(), &origF2, std::move(n2), std::move(p2));
  auto r2 = flamingo::Install(std::move(hF2));
  if (r2.has_value()) ERROR("Expected second final install to fail but it succeeded");
}


static void test_five_hook_order() {
  puts("Test: five-hook priority ordering");
  uintptr_t h1 = 0x50010001;
  uintptr_t h2 = 0x50020002;
  uintptr_t h3 = 0x50030003;
  uintptr_t h4 = 0x50040004;
  uintptr_t h5 = 0x50050005;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6 };

  auto hook_target = perform_far_hook_test(h1, to_hook);
  void* orig1 = nullptr; void* orig2 = nullptr; void* orig3 = nullptr; void* orig4 = nullptr; void* orig5 = nullptr;

  // Simpler acyclic chain: h1 -> h2 -> h3 -> h4 -> h5
  HookNameMetadata m1; m1.name = "h1";
  HookNameMetadata m2; m2.name = "h2";
  HookNameMetadata m3; m3.name = "h3";
  HookNameMetadata m4; m4.name = "h4";
  HookNameMetadata m5; m5.name = "h5";

  HookPriority p2; p2.afters.push_back(m1); // h2 after h1
  HookPriority p3; p3.afters.push_back(m2); // h3 after h2
  HookPriority p4; p4.afters.push_back(m3); // h4 after h3
  HookPriority p5; p5.afters.push_back(m4); // h5 after h4

  // Install in scrambled order to ensure priorities drive final order: 3,5,2,4,1
  flamingo::HookInfo hh3((void*)h3, hook_target.data(), &orig3, std::move(m3), std::move(p3));
  if (!flamingo::Install(std::move(hh3)).has_value()) ERROR("Failed to install h3");

  flamingo::HookInfo hh5((void*)h5, hook_target.data(), &orig5, std::move(m5), std::move(p5));
  if (!flamingo::Install(std::move(hh5)).has_value()) ERROR("Failed to install h5");

  flamingo::HookInfo hh2((void*)h2, hook_target.data(), &orig2, std::move(m2), std::move(p2));
  if (!flamingo::Install(std::move(hh2)).has_value()) ERROR("Failed to install h2");

  flamingo::HookInfo hh4((void*)h4, hook_target.data(), &orig4, std::move(m4), std::move(p4));
  if (!flamingo::Install(std::move(hh4)).has_value()) ERROR("Failed to install h4");

  flamingo::HookInfo hh1((void*)h1, hook_target.data(), &orig1, std::move(m1), HookPriority{});
  if (!flamingo::Install(std::move(hh1)).has_value()) ERROR("Failed to install h1");

  auto fixup_res = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target.data()));
  if (!fixup_res.has_value()) ERROR("Failed to get fixup pointer");
  void* fixup_ptr = (void*)fixup_res.value().data();

  // Validate expected chain: reconstruct ordering by following orig pointers and ensure it equals [h1,h2,h3,h4,h5]
  std::vector<uintptr_t> hooks = {h1, h2, h3, h4, h5};
  std::unordered_map<uintptr_t, uintptr_t> orig_map;
  orig_map[h1] = (uintptr_t)orig1;
  orig_map[h2] = (uintptr_t)orig2;
  orig_map[h3] = (uintptr_t)orig3;
  orig_map[h4] = (uintptr_t)orig4;
  orig_map[h5] = (uintptr_t)orig5;

  // find head: hook address not present in any orig_map values
  std::unordered_set<uintptr_t> pointed;
  for (auto const& kv : orig_map) {
    if (std::find(hooks.begin(), hooks.end(), kv.second) != hooks.end()) pointed.insert(kv.second);
  }
  uintptr_t head = 0;
  for (auto h : hooks) {
    if (pointed.find(h) == pointed.end()) { head = h; break; }
  }
  if (head == 0) ERROR("5-hook: could not determine head of hook chain");

  // traverse
  std::vector<uintptr_t> order;
  uintptr_t cur = head;
  while (true) {
    order.push_back(cur);
    auto it = orig_map.find(cur);
    if (it == orig_map.end()) break;
    uintptr_t next = it->second;
    if (next == (uintptr_t)fixup_ptr) break;
    cur = next;
    if (order.size() > hooks.size()) break;
  }

  std::vector<uintptr_t> expected = {h1, h2, h3, h4, h5};
  if (order != expected) {
    auto join_hex = [](std::vector<uintptr_t> const& v){
      std::string out;
      for (size_t i=0;i<v.size();++i){
        if (i) out += ", ";
        out += fmt::format("0x{:x}", v[i]);
      }
      return out;
    };
    ERROR("5-hook: ordering mismatch; expected {} but got {}", join_hex(expected), join_hex(order));
  }
}

// Forward declarations for additional edge-case tests

static void test_no_constraints_multiple() {
  puts("Test: no-constraints multiple installs");
  uintptr_t h1 = 0x60010001;
  uintptr_t h2 = 0x60020002;
  uintptr_t h3 = 0x60030003;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8 };

  auto hook_target = perform_far_hook_test(h1, to_hook);
  void* orig1 = nullptr; void* orig2 = nullptr; void* orig3 = nullptr;

  HookNameMetadata m1; m1.name = "h1";
  HookNameMetadata m2; m2.name = "h2";
  HookNameMetadata m3; m3.name = "h3";

  if (!flamingo::Install(flamingo::HookInfo((void*)h1, hook_target.data(), &orig1, std::move(m1), HookPriority{})).has_value())
    ERROR("Failed to install h1");
  if (!flamingo::Install(flamingo::HookInfo((void*)h2, hook_target.data(), &orig2, std::move(m2), HookPriority{})).has_value())
    ERROR("Failed to install h2");
  if (!flamingo::Install(flamingo::HookInfo((void*)h3, hook_target.data(), &orig3, std::move(m3), HookPriority{})).has_value())
    ERROR("Failed to install h3");

  auto fixup_res = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target.data()));
  if (!fixup_res.has_value()) ERROR("Failed to get fixup pointer");
  void* fixup_ptr = (void*)fixup_res.value().data();

  std::vector<uintptr_t> hooks = {h1,h2,h3};
  std::unordered_map<uintptr_t, uintptr_t> orig_map;
  orig_map[h1] = (uintptr_t)orig1;
  orig_map[h2] = (uintptr_t)orig2;
  orig_map[h3] = (uintptr_t)orig3;

  std::unordered_set<uintptr_t> pointed;
  for (auto const& kv : orig_map) {
    if (std::find(hooks.begin(), hooks.end(), kv.second) != hooks.end()) pointed.insert(kv.second);
  }
  uintptr_t head = 0;
  for (auto h : hooks) if (pointed.find(h) == pointed.end()) { head = h; break; }
  if (head == 0) ERROR("No-constraints: could not determine head");

  std::vector<uintptr_t> order; uintptr_t cur = head;
  while (true) {
    order.push_back(cur);
    auto it = orig_map.find(cur);
    if (it == orig_map.end()) break;
    uintptr_t next = it->second;
    if (next == (uintptr_t)fixup_ptr) break;
    cur = next;
    if (order.size() > hooks.size()) break;
  }

  std::vector<uintptr_t> expected = {h3,h2,h1};
  if (order != expected) ERROR("No-constraints: expected {} but got {}",
                              fmt::format("0x{:x},0x{:x},0x{:x}", expected[0],expected[1],expected[2]),
                              fmt::format("0x{:x}", order.empty() ? 0 : order[0]));
}

static void test_befores_namespace_multiple() {
  puts("Test: befores matching multiple in namespace");
  uintptr_t a1 = 0x70010001;
  uintptr_t a2 = 0x70020002;
  uintptr_t prior = 0x70030003;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8 };

  auto hook_target = perform_far_hook_test(a1, to_hook);
  void* origA1 = nullptr; void* origA2 = nullptr; void* origPrior = nullptr;

  HookNameMetadata ma1; ma1.name = "one"; ma1.namespaze = "grp";
  HookNameMetadata ma2; ma2.name = "two"; ma2.namespaze = "grp";

  if (!flamingo::Install(flamingo::HookInfo((void*)a1, hook_target.data(), &origA1, std::move(ma1), HookPriority{})).has_value())
    ERROR("Failed to install a1");
  if (!flamingo::Install(flamingo::HookInfo((void*)a2, hook_target.data(), &origA2, std::move(ma2), HookPriority{})).has_value())
    ERROR("Failed to install a2");

  HookNameMetadata prior_name; prior_name.name = "prior";
  HookPriority prior_p; HookNameMetadata match_ns; match_ns.namespaze = "grp"; prior_p.befores.push_back(match_ns);
  if (!flamingo::Install(flamingo::HookInfo((void*)prior, hook_target.data(), &origPrior, std::move(prior_name), std::move(prior_p))).has_value())
    ERROR("Failed to install prior");

  auto fixup_res = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target.data()));
  if (!fixup_res.has_value()) ERROR("Failed to get fixup pointer");
  void* fixup_ptr = (void*)fixup_res.value().data();

  // Expect prior -> a2 -> a1 (newer installs at front)
  if ((uintptr_t)origPrior != a2) ERROR("Befores-multi: expected prior.orig == a2 (0x{:x}) got 0x{:x}", a2, (uintptr_t)origPrior);
  if ((uintptr_t)origA2 != a1) ERROR("Befores-multi: expected a2.orig == a1 (0x{:x}) got 0x{:x}", a1, (uintptr_t)origA2);
  if ((uintptr_t)origA1 != (uintptr_t)fixup_ptr) ERROR("Befores-multi: expected a1.orig == fixup got 0x{:x}", (uintptr_t)origA1);
}

static void test_afters_namespace_multiple() {
  puts("Test: afters matching multiple in namespace");
  uintptr_t g1 = 0x80010001;
  uintptr_t g2 = 0x80020002;
  uintptr_t late = 0x80030003;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8 };

  auto hook_target = perform_far_hook_test(g1, to_hook);
  void* origG1 = nullptr; void* origG2 = nullptr; void* origLate = nullptr;

  HookNameMetadata mg1; mg1.name = "g1"; mg1.namespaze = "grp";
  HookNameMetadata mg2; mg2.name = "g2"; mg2.namespaze = "grp";

  if (!flamingo::Install(flamingo::HookInfo((void*)g1, hook_target.data(), &origG1, std::move(mg1), HookPriority{})).has_value())
    ERROR("Failed to install g1");
  if (!flamingo::Install(flamingo::HookInfo((void*)g2, hook_target.data(), &origG2, std::move(mg2), HookPriority{})).has_value())
    ERROR("Failed to install g2");

  HookNameMetadata late_name; late_name.name = "late";
  HookPriority late_p; HookNameMetadata match_ns2; match_ns2.namespaze = "grp"; late_p.afters.push_back(match_ns2);
  if (!flamingo::Install(flamingo::HookInfo((void*)late, hook_target.data(), &origLate, std::move(late_name), std::move(late_p))).has_value())
    ERROR("Failed to install late");

  auto fixup_res = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target.data()));
  if (!fixup_res.has_value()) ERROR("Failed to get fixup pointer");
  void* fixup_ptr = (void*)fixup_res.value().data();

  // Expect g2 -> g1 -> late
  if ((uintptr_t)origG2 != g1) ERROR("Afters-multi: expected g2.orig == g1 (0x{:x}) got 0x{:x}", g1, (uintptr_t)origG2);
  if ((uintptr_t)origG1 != late) ERROR("Afters-multi: expected g1.orig == late (0x{:x}) got 0x{:x}", late, (uintptr_t)origG1);
  if ((uintptr_t)origLate != (uintptr_t)fixup_ptr) ERROR("Afters-multi: expected late.orig == fixup got 0x{:x}", (uintptr_t)origLate);
}

int main() {
  test_name_matching();
  test_namespaze_matching();
  test_priority_cycle();
  test_complex_namespace();
  test_final_conflict();
  test_five_hook_order();
  test_no_constraints_multiple();
  test_befores_namespace_multiple();
  test_afters_namespace_multiple();
  puts("SORTED HOOKS TESTS PASSED");
}
