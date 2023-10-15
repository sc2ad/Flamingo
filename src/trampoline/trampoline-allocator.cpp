#include "trampoline-allocator.hpp"
#include <sys/mman.h>
#include <cstdlib>
#include <list>
#include "trampoline.hpp"
#include "util.hpp"

namespace {
struct PageType {
    void* ptr;
    std::size_t used_size;
    uint16_t trampoline_count;

    constexpr PageType(void* p, std::size_t used) : ptr(p), used_size(used), trampoline_count(1) {}
};

std::list<PageType> pages;
constexpr static std::size_t PageSize = 4096;
}  // namespace

namespace flamingo {

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
            Trampoline to_ret(reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(page.ptr) + page.used_size), trampolineSize,
                              page.used_size);
            page.used_size += trampolineSize;
            page.trampoline_count++;
            // Log allocated trampoline here
            return to_ret;
        }
    }
    // No pages with enough space available.
    void* ptr;
    if (::posix_memalign(&ptr, PageSize, PageSize) != 0) {
        // Log error on memalign allocation!
        FLAMINGO_ABORT("Failed to allocate trampoline page of size: {} for size: {}. err: {}", PageSize, trampolineSize,
                       std::strerror(errno));
    }
    // Mark full page as rxw
    if (::mprotect(ptr, PageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        // Log error on mprotect!
        FLAMINGO_ABORT("Failed to mark allocated page at: {} as +rwx. err: {}", fmt::ptr(ptr), std::strerror(errno));
    }
    auto& page = pages.emplace_back(ptr, trampolineSize);
    return { static_cast<uint32_t*>(ptr), trampolineSize, page.used_size };
}

void TrampolineAllocator::Free(Trampoline const& toFree) {
    // Freeing a trampoline should decrease the page it was allocated on's size by a known amount
    // If we reach a point where a trampoline was deallocated on a page and it was the last one in that page, then we should
    // 1. Mark the page as read/write only
    // 2. deallocate the page

    // Find page we are allocated on
    auto page_addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(toFree.address.data()) & ~(PageSize - 1));
    for (auto& p : pages) {
        if (p.ptr == page_addr) {
            p.trampoline_count--;
            if (p.trampoline_count == 0) {
                if (::mprotect(p.ptr, PageSize, PROT_READ) != 0) {
                    // Log error on mprotect
                    FLAMINGO_ABORT("Failed to mark page at: {} as read only. err: {}", fmt::ptr(p.ptr), std::strerror(errno));
                }
                ::free(p.ptr);
            }
            return;
        }
    }
    // If we get here, we couldn't free the provided Trampoline!
    FLAMINGO_ABORT("Failed to free trampoline at: {}, no matching page with page addr: {}!", fmt::ptr(toFree.address.data()),
                   fmt::ptr(page_addr));
}

}  // namespace flamingo
