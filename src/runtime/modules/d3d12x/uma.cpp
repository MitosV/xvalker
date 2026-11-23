#include "runtime/modules/d3d12x/uma.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace xv::runtime::modules::d3d12x {
namespace {
using Microsoft::WRL::ComPtr;

// allocation granularity (as VirtualAlloc)
constexpr std::size_t grain = 0x1'0000;
// largest region Direct3D 12 opens as a heap (exactly 4 GiB is refused)
constexpr std::size_t max_window = (4096 << 20) - grain;
// distance between two windows
constexpr std::uint64_t window_span = 1ull << 32;
// how many windows there are
constexpr std::uint32_t max_windows = 4;
// the console keeps 64 KiB of null addresses and 512 MiB of 32-bit descriptor heaps at the
// start of window 0; title allocations begin after them
constexpr std::size_t descriptor_headroom = (512 << 20) + grain;

// The windows of the unified memory: the memory itself and the heap opened over it.
struct Window {
    std::uintptr_t base = 0;      // where the memory is
    std::size_t size = 0;         // how much of it
    std::uint64_t va = 0;         // the host's GPU address of the window
    ComPtr<ID3D12Heap> heap;      // the heap opened over the memory
    ComPtr<ID3D12Resource> mark;  // a small resource at the end, from which `va` is read
    std::map<std::size_t, std::size_t> free; // free offsets in the window
};

// The unified memory of the process: the windows and what the title has of them.
struct Uma {
    Window windows[max_windows];
    std::uint32_t count = 0;                  // how many windows there are
    std::uintptr_t base = 0;                  // the base of the first one
    std::size_t size = 0;                     // the bytes in one window
    std::map<std::uintptr_t, std::size_t> allocated; // what the title has, by address
    std::mutex mutex;
};

Uma &uma() {
    static Uma instance;
    return instance;
}

ID3D12Device *the_host_device() { return d3d12x::host_device(); }

std::string backend_name() {
    const char *const text = std::getenv("XV_SHADER_BACKEND");
    return text ? text : "";
}

// An aligned block of a window's free list. The caller holds the mutex.
std::size_t take_from(Window &window, std::size_t size, std::uint64_t alignment) {
    const auto align = static_cast<std::size_t>(std::max<std::uint64_t>(alignment, grain));
    for (auto it = window.free.begin(); it != window.free.end(); ++it) {
        const auto start = it->first, length = it->second;
        const auto at = (start + align - 1) & ~(align - 1);
        if (at < start || at + size > start + length)
            continue;
        window.free.erase(it);
        if (at > start)
            window.free[start] = at - start;
        if (at + size < start + length)
            window.free[at + size] = start + length - (at + size);
        return at;
    }
    return 0;
}

// Give a block back, joining it to the free ranges next to it. The caller holds the mutex.
void give_to(Window &window, std::size_t offset, std::size_t size) {
    if (!size)
        return;
    auto next = window.free.upper_bound(offset);
    if (next != window.free.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == offset) {
            offset = previous->first;
            size += previous->second;
            window.free.erase(previous);
        }
    }
    if (next != window.free.end() && offset + size == next->first) {
        size += next->second;
        window.free.erase(next);
    }
    window.free[offset] = size;
}

Window *window_at(Uma &state, std::uintptr_t address, std::size_t size = 1) {
    for (std::uint32_t k = 0; k < state.count; ++k) {
        auto &window = state.windows[k];
        if (address < window.base || address - window.base > window.size ||
            size > window.size - (address - window.base))
            continue;
        return &window;
    }
    return nullptr;
}

// Four 4 GiB-aligned windows starting at an address the console's ABI can name: the low two
// bits of (address >> 32) must be zero at the first one.
std::uintptr_t reserve_windows(std::size_t size) {
    std::uintptr_t slots[max_windows]{};
    for (std::uintptr_t slot = 4; slot + max_windows <= 512; slot += max_windows) {
        std::uint32_t taken = 0;
        for (; taken < max_windows; ++taken) {
            const auto base = (slot + taken) << 32;
            slots[taken] = reinterpret_cast<std::uintptr_t>(
                VirtualAlloc(reinterpret_cast<void *>(base), size, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE));
            if (slots[taken] != base)
                break;
        }
        if (taken == max_windows)
            return slot << 32;
        for (std::uint32_t k = 0; k < taken; ++k)
            if (slots[k])
                VirtualFree(reinterpret_cast<void *>(slots[k]), 0, MEM_RELEASE);
    }
    return 0;
}
} // namespace

bool xcl_backend() { return backend_name() == "xcl"; }

bool uma_enabled() {
    static const bool enabled = [] {
        const bool on = xcl_backend();
        std::fprintf(stderr, "[Runtime] shader backend: %s%s\n",
                     backend_name().empty() ? "the ordinary one" : backend_name().c_str(),
                     on ? " (unified memory on)" : "");
        return on;
    }();
    return enabled;
}

std::uint32_t uma_megabytes() {
    static const std::uint32_t megabytes = [] {
        const char *const text = std::getenv("XV_UMA_MB");
        if (!text)
            return 1024u;
        const auto value = std::strtoul(text, nullptr, 10);
        return value ? static_cast<std::uint32_t>(value) : 1024u;
    }();
    return megabytes;
}

bool uma_reserve() {
    if (!uma_enabled())
        return false;
    auto &state = uma();
    {
        const std::lock_guard lock(state.mutex);
        if (state.count)
            return true;
    }
    auto *const device = the_host_device();
    if (!device)
        return false;
    ComPtr<ID3D12Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3)))) {
        std::fprintf(stderr, "[Runtime] UMA: the host's device has no ID3D12Device3\n");
        return false;
    }
    const auto wanted = static_cast<std::size_t>(uma_megabytes()) << 20;
    const auto size = std::min(wanted ? (wanted + grain - 1) & ~(grain - 1) : grain, max_window);
    const auto base = reserve_windows(size);
    if (!base) {
        std::fprintf(stderr, "[Runtime] UMA: no 16 GiB-aligned place for %u MiB windows\n",
                     uma_megabytes());
        return false;
    }
    std::array<ComPtr<ID3D12Heap>, max_windows> heaps;
    std::array<ComPtr<ID3D12Resource>, max_windows> marks;
    for (std::uint32_t k = 0; k < max_windows; ++k) {
        const auto address = reinterpret_cast<void *>(base + k * window_span);
        const auto result = device3->OpenExistingHeapFromAddress(address, IID_PPV_ARGS(&heaps[k]));
        if (FAILED(result)) {
            std::fprintf(stderr, "[Runtime] UMA: OpenExistingHeapFromAddress at %llx: 0x%08lx\n",
                         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)),
                         static_cast<unsigned long>(result));
            for (std::uint32_t j = 0; j < k; ++j)
                VirtualFree(reinterpret_cast<void *>(base + j * window_span), 0, MEM_RELEASE);
            VirtualFree(address, 0, MEM_RELEASE);
            return false;
        }
        // A small resource at the end gives the heap's GPU address without taking memory the
        // title can use. A placed resource has `heap VA + offset`, so the window starts at
        // the mark's address minus its own offset.
        D3D12_RESOURCE_DESC mark{};
        mark.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        mark.Width = 64;
        mark.Height = 1;
        mark.DepthOrArraySize = 1;
        mark.MipLevels = 1;
        mark.SampleDesc.Count = 1;
        mark.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        mark.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                     D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        const auto placed = device->CreatePlacedResource(heaps[k].Get(), size - grain, &mark,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&marks[k]));
        if (FAILED(placed))
            std::fprintf(stderr, "[Runtime] UMA: the mark of window %u: 0x%08lx\n", k,
                         static_cast<unsigned long>(placed));
    }
    const std::lock_guard lock(state.mutex);
    state.base = base;
    state.size = size;
    for (std::uint32_t k = 0; k < max_windows; ++k) {
        auto &window = state.windows[k];
        window = {base + k * window_span, size, 0, heaps[k], marks[k], {}};
        // the mark owns its 64 KiB, and the descriptors of window 0 are not the title's
        const auto start = k ? 0 : std::min(descriptor_headroom, size - grain);
        window.free[start] = size - grain - start;
        if (marks[k])
            window.va = marks[k]->GetGPUVirtualAddress() - (size - grain);
        std::fprintf(stderr, "[Runtime] UMA: window %u %zu MiB at %llx, heap %llu bytes, GPU "
                             "address %llx\n",
                     k, size >> 20, static_cast<unsigned long long>(window.base),
                     static_cast<unsigned long long>(heaps[k]->GetDesc().SizeInBytes),
                     static_cast<unsigned long long>(window.va));
    }
    state.count = max_windows;
    return true;
}

std::uintptr_t uma_base() { return uma().base; }
std::size_t uma_size() { return uma().size; }

std::uint32_t uma_window_count() {
    const std::lock_guard lock(uma().mutex);
    return uma().count;
}

bool uma_contains(std::uintptr_t address) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    return window_at(state, address) != nullptr;
}

bool uma_window_of(std::uintptr_t address, std::uint32_t &window, std::uint32_t &offset) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    const auto *const found = window_at(state, address);
    if (!found)
        return false;
    window = static_cast<std::uint32_t>(found - state.windows);
    offset = static_cast<std::uint32_t>(address - found->base);
    return true;
}

void *uma_alloc(std::size_t size, std::uint64_t alignment) {
    if (!uma_reserve())
        return nullptr;
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    const auto bytes = std::max<std::size_t>(grain, (size + grain - 1) & ~(grain - 1));
    const auto align = std::bit_ceil(std::max<std::uint64_t>(alignment, grain));
    for (std::uint32_t k = 0; k < state.count; ++k) {
        const auto offset = take_from(state.windows[k], bytes, align);
        if (!offset)
            continue;
        const auto address = state.windows[k].base + offset;
        state.allocated[address] = bytes;
        if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_TRACE") && shown++ < 16)
            std::fprintf(stderr, "[Runtime] UMA: %zu bytes at %llx\n", size,
                         static_cast<unsigned long long>(address));
        return reinterpret_cast<void *>(address);
    }
    std::fprintf(stderr, "[Runtime] UMA: no room for %zu bytes\n", size);
    return nullptr;
}

void *uma_alloc_at(std::uintptr_t address, std::size_t size) {
    if (!uma_reserve())
        return nullptr;
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    auto *const window = window_at(state, address, size);
    if (!window)
        return nullptr;
    const auto offset = address - window->base;
    const auto bytes = std::max<std::size_t>(grain, (size + grain - 1) & ~(grain - 1));
    const auto found = window->free.upper_bound(offset);
    if (found == window->free.begin() || offset < std::prev(found)->first ||
        offset + bytes > std::prev(found)->first + std::prev(found)->second)
        return nullptr;
    const auto previous = std::prev(found);
    const auto end = offset + bytes;
    window->free.erase(previous);
    if (offset > previous->first)
        window->free[previous->first] = offset - previous->first;
    if (end < previous->first + previous->second)
        window->free[end] = previous->first + previous->second - end;
    state.allocated[address] = bytes;
    return reinterpret_cast<void *>(address);
}

void uma_free(void *address) {
    if (!address)
        return;
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    const auto found = state.allocated.find(at);
    if (found == state.allocated.end())
        return;
    if (auto *const window = window_at(state, at))
        give_to(*window, at - window->base, found->second);
    state.allocated.erase(found);
}

std::uint64_t uma_window_va(std::uint32_t window) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    if (window >= state.count)
        return 0;
    return state.windows[window].va;
}

bool uma_on_gpu() {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    for (std::uint32_t k = 0; k < state.count; ++k)
        if (state.windows[k].va)
            return true;
    return false;
}

std::uint64_t uma_host_address(std::uintptr_t address) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    const auto *const window = window_at(state, address);
    if (!window || !window->va)
        return 0;
    return window->va + (address - window->base);
}

bool uma_heap_of(std::uintptr_t address, ID3D12Heap *&heap, std::uint64_t &offset) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    const auto *const window = window_at(state, address);
    if (!window)
        return false;
    heap = window->heap.Get();
    offset = address - window->base;
    return true;
}
} // namespace xv::runtime::modules::d3d12x
