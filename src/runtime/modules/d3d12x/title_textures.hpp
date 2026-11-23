#pragma once
#include <cstddef>
#include <cstdint>
#include <d3d12.h>

// Textures the title writes with the CPU: on the console their memory is what the GPU
// samples, laid out as the console's layout library says. Here a PC texture holds a detiled
// copy, uploaded before the title's next ExecuteCommandLists once the CPU wrote any of its
// pages — the design of XCL's hle/d3d12x/title_textures.rs (vendored under third_party/xcl),
// with the layout and detiling of xcl_tiling.dll (tools/xcl-tiling). Without that DLL, every
// path keeps today's behavior.
namespace xv::runtime::modules::d3d12x::title_textures {
// whether xcl_tiling.dll is there (loaded lazily on first use, from our own folder)
bool available();
// the description a texture was created with (the title's own, with its Layout field) and
// its state: the console layout is computed from these, not the host's description
void note_desc(ID3D12Resource *resource, const D3D12_RESOURCE_DESC &desc,
               D3D12_RESOURCE_STATES state);
// the bytes the texture takes in console layout, 0 when it cannot be computed
std::uint64_t console_bytes(ID3D12Resource *resource);
// a scratch-mapped texture (Map of a default-heap one): `memory` is the write-watched
// scratch of `bytes` the title writes; kept in step from now on
bool track_scratch(ID3D12Resource *resource, void *memory, std::uint64_t bytes);
// a placed texture (CreatePlacedResourceX): the title's memory at `address`, page-protected
// between uploads (write-watch is impossible on the title's physical memory)
bool track_placed(ID3D12Resource *resource, std::uint64_t address,
                  const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state);
// the title's GPU work writes into the resource (a copy into it): its CPU view is stale
// from then on and is not uploaded again
void note_gpu_write(ID3D12Resource *resource);
// before the title's ExecuteCommandLists: upload what the CPU wrote since the last look,
// pending first uploads included, batched on one list of the title's own direct queue
void refresh(ID3D12CommandQueue *queue);
// before the host writes [address, address + bytes) on the title's behalf (ReadFile into
// it): a write there from the kernel fails instead of faulting, so the entries in it are
// let go and marked dirty
void prepare_write(const void *address, std::size_t bytes);
// the title let the resource go: drop our reference and the layout with it
void forget(ID3D12Resource *resource);
} // namespace xv::runtime::modules::d3d12x::title_textures