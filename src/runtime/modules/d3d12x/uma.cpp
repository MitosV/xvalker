#include "runtime/modules/d3d12x/uma.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
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
constexpr std::size_t window_span = 1ull << 32;
// how many windows there are
constexpr std::uint32_t max_windows = 4;

// The windows of the unified memory: the memory itself and the heap opened over it.
struct Window {
    std::uintptr_t base = 0;  // where the memory is
    std::size_t size = 0;     // how much of it
    std::uint64_t va = 0;     // the host's GPU address of the window
    ComPtr<ID3D12Heap> heap;  // the heap opened over the memory
};

// The unified memory of the process: the windows and what the title has of them.
struct Uma {
    Window windows[max_windows];
    std::uint32_t count = 0;              // how many windows there are
    std::uintptr_t base = 0;              // the base of the first one
    std::size_t size = 0;                 // the bytes of them all
    // free ranges of the coherent window: offset → length
    std::map<std::size_t, std::size_t> free;
    std::mutex mutex;
};

Uma &uma() {
    static Uma instance;
    return instance;
}

// The host's device, when the runtime has one.
ID3D12Device *the_host_device() { return d3d12x::host_device(); }

std::string backend_name() {
    const char *const text = std::getenv("XV_SHADER_BACKEND");
    return text ? text : "";
}

// The first free 16 GiB-aligned address from 16 GiB up that a window of `size` bytes fits
// in, and that Windows can reserve there.
std::uintptr_t reserve_window(std::size_t size) {
    for (std::uintptr_t slot = 4; slot < 512; slot += max_windows) {
        const auto base = slot << 32;
        auto *const memory = VirtualAlloc(reinterpret_cast<void *>(base), size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!memory)
            continue;
        if (reinterpret_cast<std::uintptr_t>(memory) == base)
            return base;
        VirtualFree(memory, 0, MEM_RELEASE);
    }
    return 0;
}

// Take a block of `size` bytes at an `alignment`-aligned offset from the coherent window.
std::size_t take(std::size_t size, std::uint64_t alignment) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    const auto align = static_cast<std::size_t>(std::max<std::uint64_t>(alignment, grain));
    for (auto it = state.free.begin(); it != state.free.end(); ++it) {
        const auto start = it->first, length = it->second;
        const auto at = (start + align - 1) & ~(align - 1);
        if (at + size > start + length)
            continue;
        state.free.erase(it);
        if (at > start)
            state.free[start] = at - start;
        if (at + size < start + length)
            state.free[at + size] = start + length - (at + size);
        return at;
    }
    return 0;
}

void give_back(std::size_t offset, std::size_t size) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    state.free[offset] = size;
}
} // namespace

bool xcl_backend() { return backend_name() == "xcl"; }

bool uma_enabled() {
    static const bool enabled = xcl_backend();
    return enabled;
}

std::uint32_t uma_megabytes() {
    static const std::uint32_t megabytes = [] {
        const char *const text = std::getenv("XV_UMA_MB");
        if (!text)
            return 512u;
        const auto value = std::strtoul(text, nullptr, 10);
        return value ? static_cast<std::uint32_t>(value) : 512u;
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
    const auto size = (wanted + grain - 1) & ~(grain - 1);
    const auto base = reserve_window(size);
    if (!base) {
        std::fprintf(stderr, "[Runtime] UMA: no 4 GiB-aligned address for %zu bytes\n", size);
        return false;
    }
    ComPtr<ID3D12Heap> heap;
    const auto result = device3->OpenExistingHeapFromAddress(reinterpret_cast<void *>(base),
                                                             IID_PPV_ARGS(&heap));
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] UMA: OpenExistingHeapFromAddress at %llx: 0x%08lx\n",
                     static_cast<unsigned long long>(base),
                     static_cast<unsigned long>(result));
        VirtualFree(reinterpret_cast<void *>(base), 0, MEM_RELEASE);
        return false;
    }
    const std::lock_guard lock(state.mutex);
    state.windows[0] = {base, size, 0, heap};
    state.count = 1;
    state.base = base;
    state.size = size;
    state.free[0] = size;
    // The GPU address of the window itself: a placed resource at its start has it. The
    // title's shaders read the window as `mem`, `mem1` …
    D3D12_RESOURCE_DESC probe{};
    probe.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    probe.Width = 64;
    probe.Height = 1;
    probe.DepthOrArraySize = 1;
    probe.MipLevels = 1;
    probe.SampleDesc.Count = 1;
    probe.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> at_start;
    if (SUCCEEDED(device->CreatePlacedResource(heap.Get(), 0, &probe,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&at_start))))
        state.windows[0].va = at_start->GetGPUVirtualAddress();
    std::fprintf(stderr, "[Runtime] UMA: %zu MiB at %llx, opened as a heap of %llu bytes, GPU "
                         "address %llx\n",
                 size >> 20, static_cast<unsigned long long>(base),
                 static_cast<unsigned long long>(heap->GetDesc().SizeInBytes),
                 static_cast<unsigned long long>(state.windows[0].va));
    return true;
}

std::uintptr_t uma_base() { return uma().base; }
std::size_t uma_size() { return uma().size; }

bool uma_contains(std::uintptr_t address) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    if (!state.count)
        return false;
    for (std::uint32_t k = 0; k < state.count; ++k)
        if (address >= state.windows[k].base && address < state.windows[k].base + state.windows[k].size)
            return true;
    return false;
}

bool uma_window_of(std::uintptr_t address, std::uint32_t &window, std::uint32_t &offset) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    if (!state.count)
        return false;
    for (std::uint32_t k = 0; k < state.count; ++k) {
        const auto &w = state.windows[k];
        if (address >= w.base && address < w.base + w.size) {
            window = k;
            offset = static_cast<std::uint32_t>(address - w.base);
            return true;
        }
    }
    return false;
}

void *uma_alloc(std::size_t size, std::uint64_t alignment) {
    if (!uma_reserve())
        return nullptr;
    const auto offset = take((size + grain - 1) & ~(grain - 1), alignment);
    if (!offset)
        return nullptr;
    auto &state = uma();
    return reinterpret_cast<void *>(state.base + offset);
}

void *uma_alloc_at(std::uintptr_t address, std::size_t size) {
    if (!uma_reserve())
        return nullptr;
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    for (std::uint32_t k = 0; k < state.count; ++k) {
        const auto &w = state.windows[k];
        if (address < w.base || address >= w.base + w.size)
            continue;
        const auto offset = address - w.base;
        if (offset + size > w.size)
            return nullptr;
        // the range must be free
        for (const auto &[start, length] : state.free)
            if (start <= offset && offset + size <= start + length)
                return nullptr;
        return reinterpret_cast<void *>(address);
    }
    return nullptr;
}

void uma_free(void *address) {
    if (!address)
        return;
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    if (!state.count)
        return;
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    if (at < state.base || at >= state.base + state.size)
        return;
    give_back(at - state.base, 0);
}

std::uint64_t uma_window_va(std::uint32_t window) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    if (window >= state.count)
        return 0;
    return state.windows[window].va;
}

bool uma_heap_of(std::uintptr_t address, ID3D12Heap *&heap, std::uint64_t &offset) {
    auto &state = uma();
    const std::lock_guard lock(state.mutex);
    if (!state.count)
        return false;
    for (std::uint32_t k = 0; k < state.count; ++k) {
        const auto &w = state.windows[k];
        if (address < w.base || address >= w.base + w.size)
            continue;
        heap = w.heap.Get();
        offset = address - w.base;
        return true;
    }
    return false;
}
} // namespace xv::runtime::modules::d3d12x
