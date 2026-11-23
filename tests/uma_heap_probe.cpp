// Probes D3D12 heaps over fully committed process memory, as used by the XCL shader backend.
// Build for 64-bit Windows and run with Proton's Wine; no window or swap chain is needed.
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>

namespace {
using Microsoft::WRL::ComPtr;

constexpr SIZE_T heap_bytes = SIZE_T{3072} << 20;
constexpr UINT64 buffer_bytes = 64 * 1024;

int fail(const char *operation, HRESULT result) {
    std::fprintf(stderr, "uma-heap-probe: %s failed: HRESULT=0x%08lx\n", operation,
                 static_cast<unsigned long>(result));
    return 1;
}

D3D12_RESOURCE_DESC buffer_desc(D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = buffer_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

HRESULT committed_buffer(ID3D12Device *device, D3D12_HEAP_TYPE type,
                         D3D12_RESOURCE_STATES state, ID3D12Resource **resource) {
    const D3D12_HEAP_PROPERTIES heap{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const auto desc = buffer_desc();
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(resource));
}

int probe(void *memory) {
    ComPtr<ID3D12Device3> device;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
        return fail("D3D12CreateDevice(ID3D12Device3)", hr);

    ComPtr<ID3D12Heap> heap;
    hr = device->OpenExistingHeapFromAddress(memory, IID_PPV_ARGS(&heap));
    if (FAILED(hr))
        return fail("OpenExistingHeapFromAddress", hr);
    const auto heap_desc = heap->GetDesc();
    if (heap_desc.SizeInBytes != heap_bytes) {
        std::fprintf(stderr, "uma-heap-probe: heap size %llu, expected %llu\n",
                     static_cast<unsigned long long>(heap_desc.SizeInBytes),
                     static_cast<unsigned long long>(heap_bytes));
        return 1;
    }

    const auto placed_desc = buffer_desc(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                         D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    ComPtr<ID3D12Resource> placed;
    hr = device->CreatePlacedResource(heap.Get(), 0, &placed_desc, D3D12_RESOURCE_STATE_COMMON,
                                      nullptr, IID_PPV_ARGS(&placed));
    if (FAILED(hr))
        return fail("CreatePlacedResource", hr);

    ComPtr<ID3D12Resource> upload;
    hr = committed_buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                          &upload);
    if (FAILED(hr))
        return fail("CreateCommittedResource(upload)", hr);
    ComPtr<ID3D12Resource> readback;
    hr = committed_buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                          &readback);
    if (FAILED(hr))
        return fail("CreateCommittedResource(readback)", hr);

    auto *title_words = static_cast<std::uint32_t *>(memory);
    for (UINT64 i = 0; i < buffer_bytes / 4; ++i)
        title_words[i] = 0x10300000u ^ static_cast<std::uint32_t>(i);
    void *mapped = nullptr;
    hr = upload->Map(0, nullptr, &mapped);
    if (FAILED(hr))
        return fail("upload.Map", hr);
    auto *upload_words = static_cast<std::uint32_t *>(mapped);
    for (UINT64 i = 0; i < buffer_bytes / 4; ++i)
        upload_words[i] = 0xc0de0000u | static_cast<std::uint32_t>(i);
    upload->Unmap(0, nullptr);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr))
        return fail("CreateCommandQueue", hr);
    ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr))
        return fail("CreateCommandAllocator", hr);
    ComPtr<ID3D12GraphicsCommandList> list;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&list));
    if (FAILED(hr))
        return fail("CreateCommandList", hr);
    list->CopyBufferRegion(readback.Get(), 0, placed.Get(), 0, buffer_bytes);
    list->CopyBufferRegion(placed.Get(), 0, upload.Get(), 0, buffer_bytes);
    hr = list->Close();
    if (FAILED(hr))
        return fail("commandList.Close", hr);
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr))
        return fail("CreateFence", hr);
    hr = queue->Signal(fence.Get(), 1);
    if (FAILED(hr))
        return fail("queue.Signal", hr);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
    hr = fence->SetEventOnCompletion(1, event);
    if (FAILED(hr)) {
        CloseHandle(event);
        return fail("fence.SetEventOnCompletion", hr);
    }
    const DWORD wait = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0)
        return fail("WaitForSingleObject(fence)",
                    wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_FAIL);

    hr = readback->Map(0, nullptr, &mapped);
    if (FAILED(hr))
        return fail("readback.Map", hr);
    const auto *readback_words = static_cast<const std::uint32_t *>(mapped);
    bool gpu_read = true;
    for (UINT64 i = 0; i < buffer_bytes / 4; ++i)
        gpu_read &= readback_words[i] == (0x10300000u ^ static_cast<std::uint32_t>(i));
    readback->Unmap(0, nullptr);
    bool cpu_saw_write = true;
    for (UINT64 i = 0; i < buffer_bytes / 4; ++i)
        cpu_saw_write &= title_words[i] == (0xc0de0000u | static_cast<std::uint32_t>(i));

    std::printf("uma-heap-probe: 3072 MiB committed at %p; heap %llu bytes\n", memory,
                static_cast<unsigned long long>(heap_desc.SizeInBytes));
    std::printf("GPU read CPU data: %s; CPU observed GPU writes: %s\n",
                gpu_read ? "yes" : "no", cpu_saw_write ? "yes" : "no");
    return gpu_read && cpu_saw_write ? 0 : 1;
}
} // namespace

int main() {
    void *memory = nullptr;
    for (std::uintptr_t slot = 4; slot < 512 && !memory; slot += 4) {
        const auto address = reinterpret_cast<void *>(slot << 32);
        memory = VirtualAlloc(address, heap_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!memory)
        return fail("VirtualAlloc(3072 MiB, MEM_RESERVE | MEM_COMMIT)",
                    HRESULT_FROM_WIN32(GetLastError()));
    const int result = probe(memory);
    VirtualFree(memory, 0, MEM_RELEASE);
    return result;
}
