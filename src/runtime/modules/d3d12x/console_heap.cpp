#include "runtime/modules/d3d12x/console_heap.hpp"
#include "runtime/modules/d3d12x/uma.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace xv::runtime::modules::d3d12x::console_heap {
namespace {
constexpr UINT image_slot = 32, sampler_slot = 16;
constexpr UINT max_shared = 1u << 20;

ResourceLookup find_resource = nullptr;
HostLookup find_host = nullptr;

struct Heap {
    void *owner = nullptr;
    ID3D12DescriptorHeap *pc_heap = nullptr;
    std::uintptr_t shadow = 0;
    UINT n = 0;
    std::uint64_t pc_cpu = 0, pc_gpu = 0;
    UINT pc_inc = 0;
    bool sampler = false;
    UINT first = 0;
    bool shared = false;
};
std::mutex heaps_mutex;
std::vector<Heap> heaps;

struct Shared {
    ID3D12Device *device = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    std::uint64_t cpu = 0, gpu = 0;
    UINT inc = 0, cap = 0, next = 0;
    std::vector<std::pair<UINT, UINT>> free;
};
std::mutex shared_mutex;
Shared shared;
bool shared_failed = false;

UINT take_range(UINT count) {
    if (count > shared.cap - shared.next)
        return UINT_MAX;
    for (auto it = shared.free.begin(); it != shared.free.end(); ++it) {
        if (it->second < count)
            continue;
        const UINT first = it->first;
        if ((it->second -= count) == 0)
            shared.free.erase(it);
        else
            it->first += count;
        return first;
    }
    const UINT first = shared.next;
    shared.next += count;
    return first;
}

bool make_shared(ID3D12Device *device) {
    if (shared.heap || shared_failed)
        return shared.heap != nullptr;
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 options{};
    UINT cap = max_shared;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &options,
                                              sizeof options)) &&
        options.MaxViewDescriptorHeapSize)
        cap = std::min(cap, options.MaxViewDescriptorHeapSize);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = cap;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap *heap = nullptr;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) {
        shared_failed = true;
        std::fprintf(stderr, "[Runtime] console heaps: no shared view heap of %u slots\n", cap);
        return false;
    }
    shared = {device, heap, heap->GetCPUDescriptorHandleForHeapStart().ptr,
              heap->GetGPUDescriptorHandleForHeapStart().ptr,
              device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
              cap, 0, {}};
    std::fprintf(stderr, "[Runtime] console heaps: one shader-visible PC heap of %u views\n", cap);
    return true;
}

Heap *heap_of(std::uintptr_t address) {
    const std::lock_guard lock(heaps_mutex);
    for (auto &heap : heaps) {
        const auto bytes = std::uint64_t{heap.n} * (heap.sampler ? sampler_slot : image_slot);
        if (address >= heap.shadow && address - heap.shadow < bytes)
            return &heap;
    }
    return nullptr;
}

// A texture's GFX10 IMG_FORMAT number and its shape, from the PC view the title made.
struct Format {
    DXGI_FORMAT dxgi;
    UINT gfx10;
};
constexpr Format formats[] = {
    {DXGI_FORMAT_R8_UNORM, 1},          {DXGI_FORMAT_R8_SNORM, 2},
    {DXGI_FORMAT_R8_UINT, 5},           {DXGI_FORMAT_R8_SINT, 6},
    {DXGI_FORMAT_R16_UNORM, 7},         {DXGI_FORMAT_R16_SNORM, 8},
    {DXGI_FORMAT_R16_UINT, 11},         {DXGI_FORMAT_R16_SINT, 12},
    {DXGI_FORMAT_R16_FLOAT, 13},        {DXGI_FORMAT_R8G8_UNORM, 14},
    {DXGI_FORMAT_R8G8_SNORM, 15},       {DXGI_FORMAT_R8G8_UINT, 18},
    {DXGI_FORMAT_R8G8_SINT, 19},        {DXGI_FORMAT_R32_UINT, 20},
    {DXGI_FORMAT_R32_SINT, 21},         {DXGI_FORMAT_R32_FLOAT, 22},
    {DXGI_FORMAT_R16G16_UNORM, 23},     {DXGI_FORMAT_R16G16_SNORM, 24},
    {DXGI_FORMAT_R16G16_UINT, 27},      {DXGI_FORMAT_R16G16_SINT, 28},
    {DXGI_FORMAT_R16G16_FLOAT, 29},     {DXGI_FORMAT_R11G11B10_FLOAT, 36},
    {DXGI_FORMAT_R10G10B10A2_UNORM, 50}, {DXGI_FORMAT_R10G10B10A2_UINT, 54},
    {DXGI_FORMAT_R8G8B8A8_UNORM, 56},   {DXGI_FORMAT_R8G8B8A8_SNORM, 57},
    {DXGI_FORMAT_R8G8B8A8_UINT, 60},    {DXGI_FORMAT_R8G8B8A8_SINT, 61},
    {DXGI_FORMAT_R32G32_UINT, 62},      {DXGI_FORMAT_R32G32_SINT, 63},
    {DXGI_FORMAT_R32G32_FLOAT, 64},     {DXGI_FORMAT_R16G16B16A16_UNORM, 65},
    {DXGI_FORMAT_R16G16B16A16_SNORM, 66}, {DXGI_FORMAT_R16G16B16A16_UINT, 69},
    {DXGI_FORMAT_R16G16B16A16_SINT, 70}, {DXGI_FORMAT_R16G16B16A16_FLOAT, 71},
    {DXGI_FORMAT_R32G32B32_FLOAT, 74},  {DXGI_FORMAT_R32G32B32A32_UINT, 75},
    {DXGI_FORMAT_R32G32B32A32_SINT, 76}, {DXGI_FORMAT_R32G32B32A32_FLOAT, 77},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 130}, {DXGI_FORMAT_BC1_UNORM, 169},
    {DXGI_FORMAT_BC1_UNORM_SRGB, 170},  {DXGI_FORMAT_BC2_UNORM, 171},
    {DXGI_FORMAT_BC2_UNORM_SRGB, 172},  {DXGI_FORMAT_BC3_UNORM, 173},
    {DXGI_FORMAT_BC3_UNORM_SRGB, 174},  {DXGI_FORMAT_BC4_UNORM, 175},
    {DXGI_FORMAT_BC4_SNORM, 176},       {DXGI_FORMAT_BC5_UNORM, 177},
    {DXGI_FORMAT_BC5_SNORM, 178},       {DXGI_FORMAT_BC6H_UF16, 179},
    {DXGI_FORMAT_BC6H_SF16, 180},       {DXGI_FORMAT_BC7_UNORM, 181},
    {DXGI_FORMAT_BC7_UNORM_SRGB, 182},
};
UINT gfx10_format(DXGI_FORMAT format) {
    for (const auto &entry : formats)
        if (entry.dxgi == format)
            return entry.gfx10;
    return 0;
}
DXGI_FORMAT dxgi_format(UINT number) {
    for (const auto &entry : formats)
        if (entry.gfx10 == number)
            return entry.dxgi;
    return DXGI_FORMAT_UNKNOWN;
}

// T# words 0 and 1: the console byte address (256-byte units) and the format/size.
std::uint64_t image_address(const std::uint32_t *w) {
    return ((std::uint64_t{w[0]} | std::uint64_t{w[1] & 0xff} << 32) << 8);
}
UINT image_format(const std::uint32_t *w) { return (w[1] >> 20) & 0x1ff; }
UINT image_width(const std::uint32_t *w) {
    return ((w[1] >> 30) & 3) + ((w[2] & 0xfff) << 2) + 1;
}
UINT image_height(const std::uint32_t *w) { return ((w[2] >> 14) & 0x3fff) + 1; }
UINT image_type(const std::uint32_t *w) { return w[3] >> 28; }
// `xcl_image_for_view` (third_party/xcl/src/gpu/desc.rs): a T# the translated shader can use,
// with the PC heap index in word 0 instead of a GPU address.
void image_words(std::uint32_t *w, UINT index, UINT format, UINT type, UINT width, UINT height,
                 UINT depth, UINT mips) {
    const UINT w1 = width ? width - 1 : 0, h1 = height ? height - 1 : 0,
               d1 = depth ? depth - 1 : 0, last = mips ? mips - 1 : 0;
    std::memset(w, 0, image_slot);
    w[0] = index;
    w[1] = (format & 0x1ff) << 20 | (w1 & 3) << 30;
    w[2] = (w1 >> 2) & 0xfff | (h1 & 0x3fff) << 14;
    w[3] = 4 | 5 << 3 | 6 << 6 | 7 << 9 | (last & 0xf) << 16 | (type & 0xf) << 28;
    w[4] = d1 & 0x1fff;
    w[5] = (last & 0xf) << 4;
}
// View dimensions and sizes of a PC view, to write the T# the console's code expects.
bool view_shape(ID3D12Resource *resource, const void *desc, D3D12_DESCRIPTOR_RANGE_TYPE kind,
                UINT &format, UINT &type, UINT &width, UINT &height, UINT &depth, UINT &mips) {
    if (!resource)
        return false;
    const auto rd = resource->GetDesc();
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return false;
    DXGI_FORMAT fmt = rd.Format;
    D3D12_SRV_DIMENSION dim = D3D12_SRV_DIMENSION_UNKNOWN;
    UINT array = rd.DepthOrArraySize;
    if (desc && kind == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
        const auto &view = *static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC *>(desc);
        fmt = view.Format == DXGI_FORMAT_UNKNOWN ? rd.Format : view.Format;
        switch (view.ViewDimension) {
        case D3D12_UAV_DIMENSION_TEXTURE1D: dim = D3D12_SRV_DIMENSION_TEXTURE1D; break;
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            dim = D3D12_SRV_DIMENSION_TEXTURE1DARRAY; array = view.Texture1DArray.ArraySize; break;
        case D3D12_UAV_DIMENSION_TEXTURE2D: dim = D3D12_SRV_DIMENSION_TEXTURE2D; break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            dim = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; array = view.Texture2DArray.ArraySize; break;
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            dim = D3D12_SRV_DIMENSION_TEXTURE3D; array = view.Texture3D.WSize; break;
        default: break;
        }
    } else if (desc) {
        const auto &view = *static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC *>(desc);
        fmt = view.Format == DXGI_FORMAT_UNKNOWN ? rd.Format : view.Format;
        dim = view.ViewDimension;
        switch (dim) {
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: array = view.Texture1DArray.ArraySize; break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: array = view.Texture2DArray.ArraySize; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: array = view.TextureCubeArray.NumCubes * 6; break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: array = view.Texture2DMSArray.ArraySize; break;
        default: break;
        }
    } else {
        switch (rd.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            dim = rd.DepthOrArraySize > 1 ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY : D3D12_SRV_DIMENSION_TEXTURE1D; break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            dim = D3D12_SRV_DIMENSION_TEXTURE3D; break;
        default:
            dim = rd.DepthOrArraySize > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D; break;
        }
    }
    format = gfx10_format(fmt);
    if (!format || dim == D3D12_SRV_DIMENSION_UNKNOWN)
        return false;
    width = static_cast<UINT>(rd.Width);
    height = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : rd.Height;
    depth = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? rd.DepthOrArraySize : 1;
    mips = rd.MipLevels ? rd.MipLevels : 1;
    switch (dim) {
    case D3D12_SRV_DIMENSION_TEXTURE1D: type = 8; break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: type = 12; depth = array; break;
    case D3D12_SRV_DIMENSION_TEXTURE2D: type = 9; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: type = 13; depth = array; break;
    case D3D12_SRV_DIMENSION_TEXTURE3D: type = 10; depth = array ? array : depth; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: type = 11; depth = 6; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: type = 11; depth = array; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS: type = 14; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: type = 15; depth = array; break;
    default: return false;
    }
    return true;
}
// A V# for a buffer the title made a view of.
void buffer_words(std::uint32_t *w, std::uint64_t address, UINT stride, UINT records,
                  UINT format) {
    w[0] = static_cast<std::uint32_t>(address);
    w[1] = (static_cast<std::uint32_t>(address >> 32) & 0xffff) | ((stride & 0x3fff) << 16);
    w[2] = records;
    w[3] = 0xfac | ((format & 0x7f) << 12) | 1 << 24 | (stride ? 0 : 3u << 28);
}
void null_words(std::uint32_t *w, UINT index, bool sampler) {
    std::memset(w, 0, sampler ? sampler_slot : image_slot);
    if (sampler)
        w[3] = index & 0xfff;
    else
        w[0] = index;
}
void write(const Slot &slot, const std::uint32_t *words) {
    std::memcpy(reinterpret_cast<void *>(slot.at), words,
                slot.sampler ? sampler_slot : image_slot);
}
void publish(const Slot &slot, bool uav) {
    if (!slot.shared_srv || !shared.device)
        return;
    const auto handle = slot.shared_srv + (uav ? shared.inc : 0);
    shared.device->CopyDescriptorsSimple(1, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(handle)},
                                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(
                                             uav ? slot.pc_uav : slot.pc_srv)},
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}
} // namespace

void set_lookups(ResourceLookup resource, HostLookup host) {
    find_resource = resource;
    find_host = host;
}
bool enabled() { return uma_enabled(); }

std::optional<Slot> slot(std::uintptr_t address) {
    auto *const found = heap_of(address);
    if (!found)
        return std::nullopt;
    const auto k = static_cast<UINT>((address - found->shadow) /
                                     (found->sampler ? sampler_slot : image_slot));
    Slot result{};
    result.at = found->shadow + std::uint64_t{k} * (found->sampler ? sampler_slot : image_slot);
    result.sampler = found->sampler;
    result.index = found->shared ? found->first + 2 * k : k;
    result.pc_srv = found->pc_cpu + std::uint64_t{k} * found->pc_inc * (found->sampler ? 1 : 2);
    result.pc_uav = result.pc_srv + found->pc_inc;
    result.pc_gpu_uav = found->pc_gpu ? found->pc_gpu + std::uint64_t{k} * 2 * found->pc_inc : 0;
    result.shared_srv = found->shared
                            ? shared.cpu + std::uint64_t{result.index} * shared.inc
                            : 0;
    return result;
}
bool contains(std::uintptr_t address) { return heap_of(address) != nullptr; }

ID3D12DescriptorHeap *view_heap() {
    const std::lock_guard lock(shared_mutex);
    return shared.heap;
}
UINT shared_size() {
    const std::lock_guard lock(shared_mutex);
    return shared.cap;
}
bool words_at(std::uintptr_t address, std::uint32_t *out, UINT count) {
    const auto *const memory = static_cast<const std::uint32_t *>(
        uma_console_pointer(address));
    if (!memory)
        return false;
    std::memcpy(out, memory, std::size_t{count} * sizeof(std::uint32_t));
    return true;
}
ID3D12DescriptorHeap *host_heap(std::uintptr_t address) {
    auto *const found = heap_of(address);
    if (!found)
        return nullptr;
    if (!found->sampler)
        return view_heap();
    return found->pc_heap;
}

std::mutex bound_mutex;
std::map<void *, std::pair<ID3D12DescriptorHeap *, ID3D12DescriptorHeap *>> bound;

void forget(ID3D12GraphicsCommandList *list) {
    const std::lock_guard lock(bound_mutex);
    bound.erase(list);
}

void bind(ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *samplers) {
    if (!list)
        return;
    const std::lock_guard lock(bound_mutex);
    const auto views = view_heap();
    if (!views)
        return;
    auto &was = bound[list];
    if (was.first == views && was.second == samplers)
        return;
    ID3D12DescriptorHeap *heaps[2]{views, samplers};
    list->SetDescriptorHeaps(samplers ? 2 : 1, heaps);
    was = {views, samplers};
}

std::uintptr_t register_heap(ID3D12Device *device, void *owner, ID3D12DescriptorHeap *pc_heap,
                             D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n, std::uint64_t pc_cpu,
                             std::uint64_t pc_gpu, UINT pc_stride, std::uint64_t *host,
                             std::uint64_t *gpu, UINT *host_stride) {
    if (!enabled() || !device || !n ||
        (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
         type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER))
        return 0;
    const bool sampler = type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    UINT first = 0;
    if (!sampler) {
        const std::lock_guard lock(shared_mutex);
        if (!make_shared(device))
            return 0;
        first = take_range(2 * n);
        if (first == UINT_MAX)
            return 0;
    }
    auto *memory = uma_alloc(std::uint64_t{n} * (sampler ? sampler_slot : image_slot), 256);
    UINT window = UINT_MAX, offset = 0;
    if (memory && (!uma_window_of(reinterpret_cast<std::uintptr_t>(memory), window, offset) || window)) {
        uma_free(memory);
        memory = nullptr;
    }
    if (!memory) {
        if (!sampler) {
            const std::lock_guard lock(shared_mutex);
            shared.free.emplace_back(first, 2 * n);
        }
        return 0;
    }
    std::memset(memory, 0, std::size_t{n} * (sampler ? sampler_slot : image_slot));
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    if (!sampler) {
        const std::lock_guard lock(shared_mutex);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (UINT k = 0; k < n; ++k) {
            const auto srv_h = D3D12_CPU_DESCRIPTOR_HANDLE{
                static_cast<SIZE_T>(shared.cpu + std::uint64_t{first + 2 * k} * shared.inc)};
            const auto uav_h = D3D12_CPU_DESCRIPTOR_HANDLE{
                static_cast<SIZE_T>(shared.cpu + std::uint64_t{first + 2 * k + 1} * shared.inc)};
            device->CreateShaderResourceView(nullptr, &srv, srv_h);
            device->CreateUnorderedAccessView(nullptr, nullptr, &uav, uav_h);
            null_words(reinterpret_cast<std::uint32_t *>(base + std::uint64_t{k} * image_slot),
                       first + 2 * k, false);
        }
    } else {
        D3D12_SAMPLER_DESC desc{};
        desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.MaxLOD = D3D12_FLOAT32_MAX;
        for (UINT k = 0; k < n; ++k) {
            const auto h = D3D12_CPU_DESCRIPTOR_HANDLE{
                static_cast<SIZE_T>(pc_cpu + std::uint64_t{k} * pc_stride)};
            device->CreateSampler(&desc, h);
            null_words(reinterpret_cast<std::uint32_t *>(base + std::uint64_t{k} * sampler_slot),
                       k, true);
        }
    }
    std::uint64_t actual_cpu = pc_cpu, actual_gpu = pc_gpu;
    UINT actual_inc = pc_stride;
    if (!sampler) {
        const std::lock_guard lock(shared_mutex);
        actual_cpu = shared.cpu + std::uint64_t{first} * shared.inc;
        actual_gpu = shared.gpu + std::uint64_t{first} * shared.inc;
        actual_inc = shared.inc;
        *host = actual_cpu;
        *gpu = actual_gpu;
        *host_stride = actual_inc;
    } else {
        *host = pc_cpu;
        *gpu = pc_gpu;
        *host_stride = pc_stride;
    }
    {
        const std::lock_guard lock(heaps_mutex);
        heaps.push_back({owner, pc_heap, base, n, actual_cpu, actual_gpu, actual_inc, sampler,
                         first, !sampler});
    }
    return base;
}

void unregister(void *owner) {
    const std::lock_guard lock(heaps_mutex);
    for (auto it = heaps.begin(); it != heaps.end(); ++it) {
        if (it->owner != owner)
            continue;
        if (!it->sampler) {
            const std::lock_guard shared_lock(shared_mutex);
            shared.free.emplace_back(it->first, 2 * it->n);
        }
        uma_free(reinterpret_cast<void *>(it->shadow));
        heaps.erase(it);
        return;
    }
}

void made_from_host(std::uintptr_t address, ID3D12Resource *resource, const void *desc,
                    D3D12_DESCRIPTOR_RANGE_TYPE type) {
    const auto found = slot(address);
    if (!found)
        return;
    if (found->sampler) {
        std::uint32_t words[4]{};
        words[3] = found->index & 0xfff;
        write(*found, words);
        return;
    }
    const bool uav = type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    publish(*found, uav);
    std::uint32_t words[image_slot / 4]{};
    const auto *const srv = static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC *>(desc);
    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
        const auto *const view = static_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC *>(desc);
        buffer_words(words, view ? view->BufferLocation : 0, 0,
                     view ? view->SizeInBytes : 0, 22);
    } else if (resource && !(srv && srv->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)) {
        UINT format = 0, gfxType = 0, width = 0, height = 0, depth = 0, mips = 0;
        if (view_shape(resource, desc, type, format, gfxType, width, height, depth, mips))
            image_words(words, found->index, format, gfxType, width, height, depth, mips);
        else
            null_words(words, found->index, false);
    } else {
        std::uint64_t console = 0, offset = 0, size = 0;
        const bool known = resource && find_host && find_host(resource, &console);
        if (known && find_resource)
            find_resource(console, &resource, &offset, &size);
        UINT stride = 0, records = 0;
        if (srv && srv->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
            stride = srv->Buffer.StructureByteStride;
            records = srv->Buffer.NumElements;
        }
        if (known)
            buffer_words(words, console, stride, records, gfx10_format(resource->GetDesc().Format));
        else
            null_words(words, found->index, false);
    }
    write(*found, words);
}

// A T# the title wrote as hardware words: make the PC SRV it names, then replace the words.
void decode_image(const Slot &slot, const std::uint32_t *words) {
    const auto type = image_type(words);
    const auto format = dxgi_format(image_format(words));
    if (type < 8 || type > 15 || format == DXGI_FORMAT_UNKNOWN) {
        null_words(const_cast<std::uint32_t *>(words), slot.index, false);
        write(slot, words);
        return;
    }
    ID3D12Resource *resource = nullptr;
    std::uint64_t offset = 0, size = 0;
    const auto address = image_address(words);
    if (!find_resource || !find_resource(address, &resource, &offset, &size))
        resource = nullptr;
    // Fields of the console T#, as in desc::image_d3d12 (third_party/xcl/src/gpu/desc.rs).
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const UINT base = (words[3] >> 12) & 0xf, last = (words[3] >> 16) & 0xf,
               levels = last >= base ? last - base + 1 : 1;
    switch (type) {
    case 8: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            view.Texture1D = {base, levels, 0};
            break;
    case 9: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D = {base, levels, 0, 0};
            break;
    case 10: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
             view.Texture3D = {base, levels, 0};
             break;
    case 11: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
             view.TextureCube = {base, levels, 0};
             break;
    case 12: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
             view.Texture1DArray = {base, levels, (words[4] >> 16) & 0x1fff,
                                              std::max(1u, (words[4] & 0x1fff) + 1), 0};
             break;
    case 13: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
             view.Texture2DArray = {base, levels, (words[4] >> 16) & 0x1fff,
                                              std::max(1u, (words[4] & 0x1fff) + 1), 0, 0};
             break;
    case 14: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; break;
    default: view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY; break;
    }
    auto *const device = heap_of(slot.at) ? shared.device : nullptr;
    if (device) {
        device->CreateShaderResourceView(resource, resource ? &view : nullptr,
                                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(slot.pc_srv)});
        publish(slot, false);
    }
    // Only word 0 (the heap index) and the high byte of word 1 (address bits) change: size,
    // format, swizzle and mips stay as the console wrote them.
    auto patched = std::vector<std::uint32_t>(words, words + image_slot / 4);
    patched[0] = slot.index;
    patched[1] &= ~0xffu;
    write(slot, patched.data());
}

void decode_buffer(const Slot &slot, const std::uint32_t *words,
                   D3D12_DESCRIPTOR_RANGE_TYPE type) {
    const std::uint64_t address = std::uint64_t{words[0]} |
                                  (std::uint64_t{words[1] & 0xffff} << 32);
    const UINT stride = (words[1] >> 16) & 0x3fff;
    const auto bytes = std::uint64_t{words[2]} * (stride ? stride : 1);
    ID3D12Resource *resource = nullptr;
    std::uint64_t offset = 0, size = 0;
    if (!find_resource || !find_resource(address, &resource, &offset, &size))
        resource = nullptr;
    auto *const device = shared.device;
    if (!device)
        return;
    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC view{address,
            static_cast<UINT>(std::min<std::uint64_t>((bytes + 255) & ~255ull, 65536))};
        device->CreateConstantBufferView(resource ? &view : nullptr,
            D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(slot.pc_srv)});
    } else {
        const bool structured = stride > 4 && offset % stride == 0;
        const UINT element = structured ? stride : 4;
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer = {static_cast<UINT>(offset / element),
                           static_cast<UINT>(std::min(size, bytes ? bytes : size) / element),
                           structured ? stride : 0, 0,
                           structured ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW};
            device->CreateUnorderedAccessView(resource, nullptr, resource ? &view : nullptr,
                D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(slot.pc_uav)});
            publish(slot, true);
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer = {static_cast<UINT>(offset / element),
                           static_cast<UINT>(std::min(size, bytes ? bytes : size) / element),
                           structured ? stride : 0,
                           structured ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW};
            device->CreateShaderResourceView(resource, resource ? &view : nullptr,
                D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(slot.pc_srv)});
            publish(slot, false);
        }
    }
    // A V# the shader reads as plain memory keeps its address verbatim.
}
void decode_sampler(const Slot &slot, const std::uint32_t *words) {
    static constexpr D3D12_TEXTURE_ADDRESS_MODE modes[] = {
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE};
    const auto magnify = (words[2] >> 20) & 3, minify = (words[2] >> 22) & 3,
               mip = (words[2] >> 26) & 3, compare = (words[0] >> 12) & 7,
               ratio = (words[0] >> 9) & 7, border = words[3] >> 30;
    const bool anisotropic = magnify >= 2 || minify >= 2, compares = compare && compare != 7;
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = static_cast<D3D12_FILTER>(
        (anisotropic ? 0x55u : (minify & 1) << 4 | (magnify & 1) << 2 | (mip == 2 ? 1u : 0u)) |
        (compares ? 0x80u : 0u));
    desc.AddressU = modes[words[0] & 7];
    desc.AddressV = modes[(words[0] >> 3) & 7];
    desc.AddressW = modes[(words[0] >> 6) & 7];
    desc.MipLODBias = static_cast<float>(static_cast<std::int32_t>(words[2] << 18) >> 18) / 256.0f;
    desc.MaxAnisotropy = anisotropic ? 1u << ratio : 1;
    desc.ComparisonFunc = compares ? static_cast<D3D12_COMPARISON_FUNC>(compare + 1)
                                   : D3D12_COMPARISON_FUNC_NEVER;
    const float colour = border == 2 ? 1.0f : 0.0f;
    desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = colour;
    desc.BorderColor[3] = border == 0 ? 0.0f : 1.0f;
    desc.MinLOD = static_cast<float>(words[1] & 0xfff) / 256.0f;
    desc.MaxLOD = mip ? static_cast<float>((words[1] >> 12) & 0xfff) / 256.0f : desc.MinLOD;
    if (shared.device)
        shared.device->CreateSampler(&desc,
            D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(slot.pc_srv)});
    auto patched = std::vector<std::uint32_t>(words, words + 4);
    patched[3] = (patched[3] & ~0xfffu) | (slot.index & 0xfff);
    write(slot, patched.data());
}

void decode(std::uintptr_t address, D3D12_DESCRIPTOR_RANGE_TYPE type) {
    const auto found = slot(address);
    if (!found)
        return;
    auto *const memory = reinterpret_cast<const std::uint32_t *>(found->at);
    std::uint32_t words[8]{};
    std::memcpy(words, memory, found->sampler ? sampler_slot : image_slot);
    // An XCL-format slot already holds a PC index, not a console word to decode again.
    if (found->sampler) {
        if ((words[3] & 0xfff) == found->index && !words[0] && !words[1] && !words[2]) {
            return; // a sampler made through the device
        }
        decode_sampler(*found, words);
        return;
    }
    if ((words[3] >> 28) >= 8 && words[0] == found->index) {
        return; // an image made through the device, or already decoded
    }
    if (!words[0] && !words[1] && !words[2] && !words[3])
        return;
    if ((words[3] >> 28) >= 8)
        decode_image(*found, words);
    else
        decode_buffer(*found, words, type);
}

bool copy(ID3D12Device *device, std::uintptr_t destination, std::uintptr_t source, UINT count,
          D3D12_DESCRIPTOR_HEAP_TYPE type) {
    const auto dst = slot(destination), src = slot(source);
    if (!dst || !src || dst->sampler != src->sampler || !device || !count)
        return false;
    const auto pc = dst->sampler ? count : 2 * count;
    device->CopyDescriptorsSimple(pc,
        D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(dst->pc_srv)},
        D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(src->pc_srv)}, type);
    const auto step = dst->sampler ? sampler_slot : image_slot;
    for (UINT k = 0; k < count; ++k) {
        const auto d = slot(destination + std::uint64_t{k} * step);
        const auto s = slot(source + std::uint64_t{k} * step);
        if (!d || !s)
            continue;
        std::uint32_t words[8]{};
        std::memcpy(words, reinterpret_cast<const void *>(s->at),
                    s->sampler ? sampler_slot : image_slot);
        if (d->sampler)
            words[3] = (words[3] & ~0xfffu) | (d->index & 0xfff);
        else if ((words[3] >> 28) >= 8)
            words[0] = d->index;
        write(*d, words);
    }
    if (!dst->sampler && dst->shared_srv)
        device->CopyDescriptorsSimple(2 * count,
            D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(dst->shared_srv)},
            D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<std::size_t>(dst->pc_srv)},
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}
} // namespace xv::runtime::modules::d3d12x::console_heap
