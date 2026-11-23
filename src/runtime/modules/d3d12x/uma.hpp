#pragma once
#include <cstddef>
#include <cstdint>
#include <d3d12.h>

// The console's unified memory (UMA). On the console the CPU and the GPU share memory and
// shaders reach it through plain addresses. With the XCL shader backend the title's graphics
// memory is kept in regions that Direct3D 12 can open as heaps
// (ID3D12Device3::OpenExistingHeapFromAddress), so the GPU works on it directly.
//
// A heap is smaller than 4 GiB, so the memory is up to four windows: window k is one region
// at `base + k * 4 GiB` with `base` 16 GiB-aligned. The window of a console GPU address is
// then `(address >> 32) & 3` and its low 32 bits are the offset in that window's heap.
namespace xv::runtime::modules::d3d12x {
// Whether the XCL shader backend is asked for (`XV_SHADER_BACKEND=xcl`). The UMA is only
// there for it: with the ordinary backend nothing of this is used.
bool xcl_backend();
// Whether the UMA is there (the backend is asked for and the memory could be reserved).
bool uma_enabled();
// The size of one window in megabytes (`XV_UMA_MB`, default 1024).
std::uint32_t uma_megabytes();
// The windows are reserved on the first use, when the device is known.
bool uma_reserve();
// The base of the windows (0 when there is no UMA).
std::uintptr_t uma_base();
// The bytes in one window (0 when there is no UMA).
std::size_t uma_size();
// How many windows are open.
std::uint32_t uma_window_count();
// Whether `address` is in one of the windows.
bool uma_contains(std::uintptr_t address);
// The window of a console address ((address >> 32) & 3) and its offset in that window.
bool uma_window_of(std::uintptr_t address, std::uint32_t &window, std::uint32_t &offset);
// A block of the coherent window for the title's graphics memory, aligned to `alignment`.
// Null when the windows are full.
void *uma_alloc(std::size_t size, std::uint64_t alignment);
// A block at a fixed address (the title asked for one).
void *uma_alloc_at(std::uintptr_t address, std::size_t size);
// Give a block back.
void uma_free(void *address);
// The host's GPU address of a window's memory (the window itself, not a block in it).
// The title's shaders read the windows as `mem`, `mem1` …
std::uint64_t uma_window_va(std::uint32_t window);
// The heap of the window a console address is in, and the offset of the address in it.
// False when the address is not in the unified memory.
bool uma_heap_of(std::uintptr_t address, ID3D12Heap *&heap, std::uint64_t &offset);
// The host's GPU address of a console address in the unified memory (0: it is not in it).
std::uint64_t uma_host_address(std::uintptr_t address);
// Whether the unified memory is open on the GPU.
bool uma_on_gpu();
} // namespace xv::runtime::modules::d3d12x
