#include <utility>
#include "calling-convention.hpp"
#include "hook-data.hpp"
#include "installer.hpp"

int to_call(int x) {}

int (*to_call_orig)(int);

void test() {
  auto result = flamingo::Install(flamingo::HookInfo{ &to_call, (void*)0x1234, &to_call_orig });

  flamingo::HookInfo my_hook{ &to_call, nullptr, &to_call_orig };
  flamingo::HookInfo info(&to_call, (void*)nullptr, &to_call_orig, flamingo::CallingConvention::Cdecl,
                          flamingo::HookNameMetadata{
                            .name = "to_call_hook",
                          });
  flamingo::Install(std::move(info));
}
