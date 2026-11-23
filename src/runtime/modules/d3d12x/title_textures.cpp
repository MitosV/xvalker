#include "runtime/modules/d3d12x/title_textures.hpp"
#include "runtime/modules/xcl_tiling.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

// Textures the title writes with the CPU, kept in step with a detiled PC copy: the design of
// XCL's hle/d3d12x/title_textures.rs (third_party/xcl/src/hle/d3d12x/title_textures.rs). A
// scratch-mapped texture's memory is ours, write-watched as XCL watches its UMA; a placed
// texture lies in the title's physical memory (section views, memory.cpp), which cannot be
// write-watched, so it is kept read-only between uploads and the first write faults into the
// vectored handler, as placement.cpp does for the same memory. The first upload is pending
// until the title's next ExecuteCommandLists, where all pending and rewritten textures go in
// one batch on its own direct queue. The layout and detiling are xcl_tiling.dll's (XCL's
// vendored tiling); without it, nothing here runs.
namespace xv::runtime::modules::d3d12x::title_textures {
namespace {
// xcl_tiling.dll, loaded lazily from the folder this module itself is in (next to xvboot.exe)
struct Api {
    uint32_t (*version)() = nullptr;
    int32_t (*create)(const uint8_t *, uint32_t, XclTiling **) = nullptr;
    void (*destroy)(XclTiling *) = nullptr;
    uint64_t (*size)(const XclTiling *) = nullptr;
    uint32_t (*subresources)(const XclTiling *) = nullptr;
    int32_t (*subresource)(const XclTiling *, uint32_t, uint32_t *, uint32_t *) = nullptr;
    int32_t (*detile)(const XclTiling *, const uint8_t *, uint64_t, uint32_t, uint8_t *,
                      uint64_t) = nullptr;
};
const Api *api() {
    static const Api loaded = [] {
        Api a{};
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&api), &module) &&
            GetModuleFileNameW(module, path, MAX_PATH)) {
            std::wstring file(path);
            if (const auto slash = file.find_last_of(L'\\'); slash != std::wstring::npos) {
                file = file.substr(0, slash + 1) + L"xcl_tiling.dll";
                module = LoadLibraryExW(file.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                            LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
        if (!module)
            module = LoadLibraryExW(L"xcl_tiling.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            return a;
        const auto find = [&](const char *name) {
            return reinterpret_cast<void *>(GetProcAddress(module, name));
        };
        a.version = reinterpret_cast<decltype(a.version)>(find("xcl_tiling_version"));
        a.create = reinterpret_cast<decltype(a.create)>(find("xcl_tiling_create"));
        a.destroy = reinterpret_cast<decltype(a.destroy)>(find("xcl_tiling_destroy"));
        a.size = reinterpret_cast<decltype(a.size)>(find("xcl_tiling_size"));
        a.subresources =
            reinterpret_cast<decltype(a.subresources)>(find("xcl_tiling_subresources"));
        a.subresource =
            reinterpret_cast<decltype(a.subresource)>(find("xcl_tiling_subresource"));
        a.detile = reinterpret_cast<decltype(a.detile)>(find("xcl_tiling_detile"));
        if (!a.version || !a.create || !a.destroy || !a.size || !a.subresources ||
            !a.subresource || !a.detile || a.version() != 1) {
            std::fprintf(stderr, "[Runtime] xcl_tiling.dll without the ABI of version 1\n");
            std::fflush(stderr);
            return Api{};
        }
        std::fprintf(stderr,
                     "[Runtime] xcl_tiling.dll: the console's texture layout, title writes "
                     "kept in step\n");
        std::fflush(stderr);
        return a;
    }();
    return loaded.create ? &loaded : nullptr;
}

// what a texture was created with: the title's own description (its Layout field names the
// console's tiling; the host's description has lost it) and the state uploads leave it in
struct Created {
    D3D12_RESOURCE_DESC desc{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
struct Entry {
    ID3D12Resource *resource = nullptr; // one reference of ours
    XclTiling *layout = nullptr;
    std::uintptr_t memory = 0;
    std::uint64_t bytes = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool placed = false;  // the title's memory: page-protected; scratch: write-watched
    bool pending = true;  // the first upload has not run
    bool dirty = false;   // the CPU wrote since the last upload
    bool gpu_written = false; // the title's GPU work wrote it: the CPU view is stale
    bool readable = true; // placed memory committed yet
};
std::mutex mutex;
std::map<ID3D12Resource *, Created> created;
std::map<ID3D12Resource *, Entry> entries;
std::atomic<std::size_t> kept{0}; // the fast path of refresh
PVOID handler = nullptr;

constexpr std::uintptr_t page = 4096;
std::uintptr_t page_down(std::uintptr_t address) { return address & ~(page - 1); }
std::uintptr_t page_up(std::uintptr_t address) { return (address + page - 1) & ~(page - 1); }
// region by region: VirtualProtect fails for a range over more than one allocation, and the
// title's physical memory is mapped as many views
void protect(std::uintptr_t start, std::uintptr_t end, DWORD protection) {
    for (auto at = page_down(start); at < page_up(end);) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof info))
            return;
        const auto region_end = std::min(
            page_up(end), reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
        DWORD previous;
        if (info.State == MEM_COMMIT)
            VirtualProtect(reinterpret_cast<void *>(at), region_end - at, protection, &previous);
        at = region_end;
    }
}
bool readable(std::uintptr_t start, std::size_t bytes) {
    for (auto at = page_down(start); at < start + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}
bool writable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
           (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
}
// the first write to a placed texture's memory since its last upload
LONG CALLBACK written(EXCEPTION_POINTERS *exception) {
    const auto *record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    const std::lock_guard lock(mutex);
    bool ours = false;
    for (auto &[resource, entry] : entries) {
        if (!entry.placed || !entry.readable)
            continue;
        if (address >= page_down(entry.memory) && address < page_up(entry.memory + entry.bytes)) {
            ours = true;
            entry.dirty = true;
            protect(entry.memory, entry.memory + entry.bytes, PAGE_READWRITE);
        }
    }
    // going on is only right if the write can now happen; otherwise it faults again forever
    if (ours && !writable(address))
        protect(address, address + 1, PAGE_READWRITE);
    return ours && writable(address) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
// let an entry go: its memory writable again, the layout and our reference dropped. The
// reference goes outside the lock: Release comes back through the title's vtable wrapper.
void release_entry(Entry &entry) {
    if (entry.placed && entry.readable)
        protect(entry.memory, entry.memory + entry.bytes, PAGE_READWRITE);
    if (entry.layout)
        api()->destroy(entry.layout);
    if (entry.resource)
        entry.resource->Release();
    entry = Entry{};
}
void warn_upload(const char *what) {
    if (static std::atomic<int> shown{0}; shown++ < 4) {
        std::fprintf(stderr, "[Runtime] title texture: %s\n", what);
        std::fflush(stderr);
    }
}

// the uploads' own list, allocator and fence, as upload_placements_d3d12 keeps them: the
// staging buffers live until the GPU is past them
struct Uploader {
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
    std::vector<ID3D12Resource *> buffers;
};
Uploader uploader; // under mutex, which refresh holds
D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
// one texture's detiled subresources onto the list: staging buffer, copies, barriers from
// its state and back to it (the state it was created in, which uploads leave it in)
void record_upload(const Api &a, ID3D12Device *device, Entry &entry) {
    const auto desc = entry.resource->GetDesc();
    const UINT count = a.subresources(entry.layout);
    if (!count)
        return;
    std::vector<std::vector<std::byte>> subs(count);
    bool ok = true;
    for (UINT k = 0; k < count && ok; ++k) {
        UINT rows = 0, row_bytes = 0;
        if (a.subresource(entry.layout, k, &rows, &row_bytes) != 0 || !rows || !row_bytes) {
            ok = false;
            break;
        }
        subs[k].resize(std::size_t{rows} * row_bytes);
        ok = a.detile(entry.layout, reinterpret_cast<const uint8_t *>(entry.memory), entry.bytes,
                      k, reinterpret_cast<uint8_t *>(subs[k].data()), subs[k].size()) == 0;
    }
    if (!ok) {
        warn_upload("the console layout does not detile");
        return;
    }
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_bytes(count);
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(),
                                  row_bytes.data(), &bytes);
    for (UINT k = 0; k < count; ++k) {
        if (subs[k].size() != std::size_t{rows[k]} * row_bytes[k]) {
            warn_upload("the host's subresources are not the console's");
            return;
        }
    }
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                          0,
                                          bytes,
                                          1,
                                          1,
                                          1,
                                          DXGI_FORMAT_UNKNOWN,
                                          {1, 0},
                                          D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                          D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *buffer = nullptr;
    void *mapped = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&buffer))) ||
        FAILED(buffer->Map(0, nullptr, &mapped))) {
        if (buffer)
            buffer->Release();
        warn_upload("no staging buffer for the upload");
        return;
    }
    for (UINT k = 0; k < count; ++k) {
        const auto pitch = footprints[k].Footprint.RowPitch;
        for (UINT row = 0; row < rows[k]; ++row)
            std::memcpy(static_cast<std::uint8_t *>(mapped) + footprints[k].Offset +
                            std::size_t{row} * pitch,
                        subs[k].data() + std::size_t{row} * row_bytes[k], row_bytes[k]);
    }
    buffer->Unmap(0, nullptr);
    if (entry.state != D3D12_RESOURCE_STATE_COPY_DEST) {
        const auto barrier =
            transition(entry.resource, entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
        uploader.list->ResourceBarrier(1, &barrier);
    }
    for (UINT k = 0; k < count; ++k) {
        const D3D12_TEXTURE_COPY_LOCATION target{entry.resource,
                                                 D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                 {.SubresourceIndex = k}};
        const D3D12_TEXTURE_COPY_LOCATION source{buffer,
                                                 D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                 {.PlacedFootprint = footprints[k]}};
        uploader.list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    }
    if (entry.state != D3D12_RESOURCE_STATE_COPY_DEST) {
        const auto barrier =
            transition(entry.resource, D3D12_RESOURCE_STATE_COPY_DEST, entry.state);
        uploader.list->ResourceBarrier(1, &barrier);
    }
    uploader.buffers.push_back(buffer);
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_TRACE") && shown++ < 8) {
        std::fprintf(stderr,
                     "[Runtime] title texture at %llx (%llux%u format %d): %u subresources "
                     "detiled and uploaded, %llu bytes of console memory\n",
                     static_cast<unsigned long long>(entry.memory), desc.Width, desc.Height,
                     desc.Format, count, static_cast<unsigned long long>(entry.bytes));
        std::fflush(stderr);
    }
}
} // namespace

bool available() { return api() != nullptr; }

void note_desc(ID3D12Resource *resource, const D3D12_RESOURCE_DESC &desc,
               D3D12_RESOURCE_STATES state) {
    if (!resource || !api() || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1)
        return;
    const std::lock_guard lock(mutex);
    created[resource] = {desc, state};
}

std::uint64_t console_bytes(ID3D12Resource *resource) {
    const auto *const a = api();
    if (!a || !resource)
        return 0;
    D3D12_RESOURCE_DESC desc{};
    {
        const std::lock_guard lock(mutex);
        const auto found = created.find(resource);
        if (found == created.end())
            return 0;
        desc = found->second.desc;
    }
    static_assert(sizeof(D3D12_RESOURCE_DESC) == 0x38, "the console's description is 0x38 bytes");
    XclTiling *layout = nullptr;
    if (a->create(reinterpret_cast<const uint8_t *>(&desc), 0, &layout) != 0 || !layout)
        return 0;
    const auto bytes = a->size(layout);
    a->destroy(layout);
    return bytes;
}

bool track_scratch(ID3D12Resource *resource, void *memory, std::uint64_t bytes) {
    const auto *const a = api();
    if (!a || !resource || !memory || !bytes)
        return false;
    Created stored{};
    {
        const std::lock_guard lock(mutex);
        const auto found = created.find(resource);
        if (found == created.end())
            return false;
        stored = found->second;
    }
    XclTiling *layout = nullptr;
    if (a->create(reinterpret_cast<const uint8_t *>(&stored.desc), 0, &layout) != 0 || !layout)
        return false;
    if (a->size(layout) > bytes) {
        a->destroy(layout);
        return false;
    }
    Entry entry{};
    entry.resource = resource;
    entry.layout = layout;
    entry.memory = reinterpret_cast<std::uintptr_t>(memory);
    entry.bytes = bytes;
    entry.state = stored.state;
    resource->AddRef();
    ResetWriteWatch(memory, static_cast<SIZE_T>(bytes));
    Entry old{};
    {
        const std::lock_guard lock(mutex);
        if (const auto found = entries.find(resource); found != entries.end()) {
            old = std::move(found->second);
            entries.erase(found);
        }
        entries[resource] = entry;
        kept.store(entries.size(), std::memory_order_relaxed);
    }
    release_entry(old);
    return true;
}

bool track_placed(ID3D12Resource *resource, std::uint64_t address,
                  const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state) {
    const auto *const a = api();
    if (!a || !resource || !address || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1)
        return false;
    XclTiling *layout = nullptr;
    if (a->create(reinterpret_cast<const uint8_t *>(&desc), 0, &layout) != 0 || !layout)
        return false;
    const auto bytes = a->size(layout);
    if (!bytes) {
        a->destroy(layout);
        return false;
    }
    Entry entry{};
    entry.resource = resource;
    entry.layout = layout;
    entry.memory = static_cast<std::uintptr_t>(address);
    entry.bytes = bytes;
    entry.state = state;
    entry.placed = true;
    // memory the title has not committed yet: looked at again whenever work is submitted
    entry.readable = readable(entry.memory, static_cast<std::size_t>(bytes));
    resource->AddRef();
    Entry old{};
    {
        const std::lock_guard lock(mutex);
        // a texture placed where an older one was replaces it
        for (auto at = entries.begin(); at != entries.end(); ++at) {
            if (at->second.placed && at->second.memory == entry.memory) {
                old = std::move(at->second);
                entries.erase(at);
                break;
            }
        }
        if (entry.readable) {
            if (!handler)
                handler = AddVectoredExceptionHandler(1, &written);
            protect(entry.memory, entry.memory + entry.bytes, PAGE_READONLY);
        }
        entries[resource] = entry;
        kept.store(entries.size(), std::memory_order_relaxed);
    }
    release_entry(old);
    return true;
}

void note_gpu_write(ID3D12Resource *resource) {
    if (!kept.load(std::memory_order_relaxed) || !resource)
        return;
    const std::lock_guard lock(mutex);
    const auto found = entries.find(resource);
    if (found != entries.end() && !found->second.gpu_written)
        found->second.gpu_written = true;
}

void refresh(ID3D12CommandQueue *queue) {
    if (!kept.load(std::memory_order_relaxed) || !queue)
        return;
    // shader resource states are a direct queue's: the uploads wait for one
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    const auto *const a = api();
    if (!a)
        return;
    const std::lock_guard lock(mutex);
    // scratch memory: what the CPU wrote since the last look (the watch resets as it reads)
    void *pages[16384];
    for (auto &[resource, entry] : entries) {
        if (entry.placed || !entry.readable)
            continue;
        ULONG_PTR count = std::size(pages);
        DWORD granularity = 0;
        while (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void *>(entry.memory),
                             static_cast<SIZE_T>(entry.bytes), pages, &count, &granularity) == 0) {
            if (count)
                entry.dirty = true;
            if (count < std::size(pages))
                break;
            count = std::size(pages);
        }
    }
    // placed memory the title has committed since the last look: followed from now on
    for (auto &[resource, entry] : entries) {
        if (!entry.placed || entry.readable)
            continue;
        if (readable(entry.memory, static_cast<std::size_t>(entry.bytes))) {
            entry.readable = true;
            entry.pending = true;
            if (!handler)
                handler = AddVectoredExceptionHandler(1, &written);
            protect(entry.memory, entry.memory + entry.bytes, PAGE_READONLY);
        }
    }
    // textures made since the last submit and those with a written page; the GPU-written
    // ones keep what the title's own work put in them
    std::vector<Entry *> todo;
    for (auto &[resource, entry] : entries)
        if (entry.readable && (entry.pending || (entry.dirty && !entry.gpu_written)))
            todo.push_back(&entry);
    if (todo.empty())
        return;
    ID3D12Device *device = nullptr;
    queue->GetDevice(IID_PPV_ARGS(&device));
    if (!device)
        return;
    if (!uploader.list &&
        (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&uploader.allocator))) ||
         FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploader.allocator,
                                          nullptr, IID_PPV_ARGS(&uploader.list))) ||
         FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploader.fence))))) {
        device->Release();
        return;
    }
    if (uploader.value) {
        if (uploader.fence->GetCompletedValue() < uploader.value) {
            auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            uploader.fence->SetEventOnCompletion(uploader.value, event);
            WaitForSingleObject(event, 5000);
            CloseHandle(event);
        }
        for (auto *buffer : uploader.buffers)
            buffer->Release();
        uploader.buffers.clear();
        uploader.allocator->Reset();
        uploader.list->Reset(uploader.allocator, nullptr);
    }
    for (auto *entry : todo) {
        record_upload(*a, device, *entry);
        entry->pending = false;
        entry->dirty = false;
        if (entry->placed)
            protect(entry->memory, entry->memory + entry->bytes, PAGE_READONLY);
    }
    uploader.list->Close();
    ID3D12CommandList *const lists[] = {uploader.list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(uploader.fence, ++uploader.value);
    device->Release();
}

void prepare_write(const void *address, std::size_t bytes) {
    if (!kept.load(std::memory_order_relaxed) || !address || !bytes)
        return;
    const std::lock_guard lock(mutex);
    const auto first = reinterpret_cast<std::uintptr_t>(address), last = first + bytes;
    for (auto &[resource, entry] : entries) {
        if (!entry.placed || !entry.readable || entry.memory >= last ||
            entry.memory + entry.bytes <= first)
            continue;
        entry.dirty = true;
        protect(entry.memory, entry.memory + entry.bytes, PAGE_READWRITE);
    }
}

void forget(ID3D12Resource *resource) {
    Entry dead{};
    {
        const std::lock_guard lock(mutex);
        created.erase(resource);
        const auto found = entries.find(resource);
        if (found == entries.end())
            return;
        dead = std::move(found->second);
        entries.erase(found);
        kept.store(entries.size(), std::memory_order_relaxed);
    }
    release_entry(dead);
}
} // namespace xv::runtime::modules::d3d12x::title_textures