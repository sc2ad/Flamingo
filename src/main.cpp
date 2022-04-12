// On dlopen, we should basically just construct our vectors and everything else
// As well as our analytics data
#include "more_stuff.hpp"
#include "hook-installer.hpp"
#include "modloader/shared/modloader.hpp"

std::vector<HookData> HookData::hooks_to_install;

void HookData::RegisterHook(HookData&& data) {
    hooks_to_install.emplace_back(std::forward<HookData>(data));
}

extern "C" void load() {
    // Here's where we will INSTALL all of our hooks!

}