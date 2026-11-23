#pragma once
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <optional>

// XCL's console descriptor model, used only with XV_SHADER_BACKEND=xcl.
//
// Translated shaders index descriptor heaps directly: T# word 0 is the index of an SRV in one
// shared shader-visible PC heap, and S# word 3 [11:0] is a sampler index. The title's console
// descriptor memory holds those words, in UMA window 0 so a 32-bit descriptor table address
// still names it. This module keeps that memory and the PC descriptors beside it.
namespace xv::runtime::modules::d3d12x::console_heap {
// The runtime finds the resource behind a console address (a buffer or the start of a texture)
// and the offset/size from there for us.
using ResourceLookup = bool (*)(std::uint64_t address, ID3D12Resource **resource,
                                std::uint64_t *offset, std::uint64_t *size);
using HostLookup = bool (*)(ID3D12Resource *resource, std::uint64_t *address);
void set_lookups(ResourceLookup resource, HostLookup host);
bool enabled();

struct Slot {
    std::uintptr_t at;
    UINT index;               // the SRV (UAV at + 1) or sampler index in the PC heap
    std::uint64_t pc_srv;     // the title's PC heap, where views are made
    std::uint64_t pc_uav;
    std::uint64_t pc_gpu_uav; // 0 when the title's heap is not shader visible
    std::uint64_t shared_srv; // CPU handle of the SRV in the shared heap, 0 when not in it
    bool sampler;
};
// The console slot at a console CPU/GPU handle.
std::optional<Slot> slot(std::uintptr_t address);
bool contains(std::uintptr_t address);
// The heap a bound console address needs: the shared view heap or the title's sampler heap.
ID3D12DescriptorHeap *host_heap(std::uintptr_t address);
// The shared shader-visible view heap, if one was made.
ID3D12DescriptorHeap *view_heap();
// Windows 0 can name the shared heap at draw time even when the title did not bind it.
void bind(ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *samplers);
// The title bound heaps itself: do not assume our last binding is still in force.
void forget(ID3D12GraphicsCommandList *list);

// Give the title's heap `owner` `n` console slots. `pc_cpu`/`pc_gpu` and `pc_stride` describe
// the host's heap the title's PC descriptors go into; the returned handles/stride replace them.
// Returns the console address of slot 0, 0 when UMA is unavailable.
std::uintptr_t register_heap(ID3D12Device *device, void *owner, ID3D12DescriptorHeap *pc_heap,
                             D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n, std::uint64_t pc_cpu,
                             std::uint64_t pc_gpu, UINT pc_stride, std::uint64_t *host,
                             std::uint64_t *gpu, UINT *host_stride);
// The heap object was released: its UMA and shared-heap range go back.
void unregister(void *owner);

// A view or sampler made through the device: copy its PC descriptor to the shared heap and put
// its XCL-format words in the console slot. `type` says which kind of view `desc` describes.
void made_from_host(std::uintptr_t address, ID3D12Resource *resource, const void *desc,
                    D3D12_DESCRIPTOR_RANGE_TYPE type);
// Decode a descriptor the title wrote straight into console memory: make the PC descriptor it
// names, then replace the raw console words with XCL-format ones.
void decode(std::uintptr_t address, D3D12_DESCRIPTOR_RANGE_TYPE type);
// Copy `count` slots, renumbering their PC indices for the destination, and copy/publish the PC
// descriptors. Both ranges are in console heaps of the same kind.
bool copy(ID3D12Device *device, std::uintptr_t destination, std::uintptr_t source, UINT count,
          D3D12_DESCRIPTOR_HEAP_TYPE type);
} // namespace xv::runtime::modules::d3d12x::console_heap
