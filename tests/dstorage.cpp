// The console's DirectStorage, as a title uses it: the factory, a queue, a
// status array and the requests in them.
#include "runtime/common/runtime.hpp"
#include "runtime/modules/dstorage.hpp"
#include <catch2/catch.hpp>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <windows.h>
#include <d3d12.h>

namespace {
constexpr GUID factory_iid{
    0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};
constexpr GUID status_iid{
    0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID queue_iid{
    0xa3f6649a, 0x8c2c, 0x4b3a, {0x8b, 0x6e, 0x10, 0x18, 0x6a, 0x04, 0xb9, 0xbc}};

// the console's header, as the runtime answers it
struct StatusArray {
    virtual HRESULT QueryInterface(REFIID, void **) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
    virtual BOOL IsComplete(UINT32 index) = 0;
    virtual HRESULT GetHResult(UINT32 index) = 0;
};
struct QueueDesc {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    const char *name;
};
struct QueueInfo {
    QueueDesc desc;
    UINT16 empty_slots, until_submit;
};
struct Request {
    UINT64 options;
    void *destination;
    UINT64 destination_page_offset;
    UINT32 destination_size;
    void *source;
    UINT64 file_offset;
    UINT32 source_size, intermediate_size;
    UINT64 tag;
    const char *name;
};
struct ErrorRecord {
    UINT32 failures;
    UINT32 reserved;
    HRESULT first;
    UINT32 command;
    UINT64 options;
    UINT64 destination;
    UINT32 destination_size;
    WCHAR file[0x104];
    UINT8 reserved2[0xc];
    UINT64 memory_source;
    UINT8 reserved3[0x50];
};
struct Queue {
    virtual HRESULT QueryInterface(REFIID, void **) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
    virtual void EnqueueRequest(const Request *request) = 0;
    virtual void EnqueueStatus(StatusArray *status, UINT32 index) = 0;
    virtual void EnqueueSignal(ID3D12Fence *fence, UINT64 value) = 0;
    virtual void Submit() = 0;
    virtual void CancelRequestsWithTag(UINT64 mask, UINT64 value) = 0;
    virtual void Close() = 0;
    virtual HANDLE GetErrorEvent() = 0;
    virtual void RetrieveErrorRecord(ErrorRecord *record) = 0;
    virtual void Query(QueueInfo *info) = 0;
};
struct Factory {
    virtual HRESULT QueryInterface(REFIID, void **) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
    virtual HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) = 0;
    virtual HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) = 0;
    virtual HRESULT CreateStatusArray(UINT32 capacity, const char *name, REFIID iid,
                                      void **status) = 0;
};

// the options of a request
constexpr UINT64 option_zlib = 1ull << 0;
constexpr UINT64 option_bcpack = 0xfull << 1;
constexpr UINT64 option_source_memory = 1ull << 22;

using GetFactory = HRESULT(WINAPI *)(const GUID *, void **);

struct Runtime {
    Factory *factory = nullptr;
    explicit Runtime() {
        xv::runtime::Registry registry;
        xv::runtime::modules::install_dstorage(registry);
        const auto function = reinterpret_cast<GetFactory>(registry.find("dstorage_xs.dll",
                                                                        "DStorageGetFactory"));
        REQUIRE(function);
        REQUIRE(SUCCEEDED(function(&factory_iid, reinterpret_cast<void **>(&factory))));
    }
    ~Runtime() { factory->Release(); }
    Queue *queue() {
        const QueueDesc desc{1, 8, 2, "test"}; // a queue of memory
        void *made = nullptr;
        REQUIRE(SUCCEEDED(factory->CreateQueue(&desc, queue_iid, &made)));
        return static_cast<Queue *>(made);
    }
    StatusArray *status(UINT32 capacity) {
        void *made = nullptr;
        REQUIRE(SUCCEEDED(factory->CreateStatusArray(capacity, "test", status_iid, &made)));
        return static_cast<StatusArray *>(made);
    }
};
} // namespace

TEST_CASE("a memory queue reads into the title's memory") {
    Runtime runtime;
    auto *const queue = runtime.queue();
    auto *const status = runtime.status(4);
    std::vector<std::uint8_t> source{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<std::uint8_t> destination(source.size(), 0);
    Request request{};
    request.options = option_source_memory;
    request.destination = destination.data();
    request.destination_size = static_cast<UINT32>(destination.size());
    request.source = source.data();
    request.source_size = static_cast<UINT32>(source.size());
    queue->EnqueueRequest(&request);
    queue->EnqueueStatus(status, 0);
    queue->Submit();
    CHECK(destination == source);
    CHECK(status->IsComplete(0) == TRUE);
    CHECK(status->GetHResult(0) == S_OK);
    queue->Release();
    status->Release();
}

TEST_CASE("a request that does not fit is refused, and the error record has it") {
    Runtime runtime;
    auto *const queue = runtime.queue();
    auto *const status = runtime.status(4);
    std::vector<std::uint8_t> source(8, 0);
    std::vector<std::uint8_t> destination(16, 0);
    Request request{};
    request.options = option_source_memory;
    request.destination = destination.data();
    request.destination_size = static_cast<UINT32>(destination.size());
    request.source = source.data();
    request.source_size = static_cast<UINT32>(source.size());
    queue->EnqueueRequest(&request);
    queue->EnqueueStatus(status, 0);
    queue->Submit();
    CHECK(status->GetHResult(0) == static_cast<HRESULT>(0x8924000fu)); // the size
    CHECK(destination == std::vector<std::uint8_t>(16, 0));
    ErrorRecord record{};
    queue->RetrieveErrorRecord(&record);
    CHECK(record.failures == 1);
    CHECK(record.first == static_cast<HRESULT>(0x8924000fu));
    CHECK(record.command == 0); // a request
    CHECK(record.options == request.options);
    CHECK(record.memory_source == reinterpret_cast<std::uint64_t>(request.source));
    queue->Release();
    status->Release();
}

TEST_CASE("a request that is cancelled does not run") {
    Runtime runtime;
    auto *const queue = runtime.queue();
    auto *const status = runtime.status(4);
    std::vector<std::uint8_t> source{9, 9, 9, 9};
    std::vector<std::uint8_t> destination(source.size(), 0);
    Request request{};
    request.options = option_source_memory;
    request.destination = destination.data();
    request.destination_size = static_cast<UINT32>(destination.size());
    request.source = source.data();
    request.source_size = static_cast<UINT32>(source.size());
    request.tag = 0x1234;
    queue->EnqueueRequest(&request);
    queue->CancelRequestsWithTag(0xffff, 0x1234);
    queue->EnqueueStatus(status, 0);
    queue->Submit();
    CHECK(destination == std::vector<std::uint8_t>(source.size(), 0));
    CHECK(status->GetHResult(0) == S_OK);
    queue->Release();
    status->Release();
}

TEST_CASE("the queue tells how much of it is free") {
    Runtime runtime;
    auto *const queue = runtime.queue();
    QueueInfo info{};
    queue->Query(&info);
    CHECK(info.desc.capacity == 8);
    CHECK(info.desc.source_type == 1);
    CHECK(info.empty_slots == 8);
    CHECK(info.until_submit == 4);
    queue->Release();
}

TEST_CASE("a BCPack chunk is decoded when the library is there") {
    // the library is built by tools/bcpack/build.sh; without it the request fails
    const auto module = LoadLibraryExW(L"bcpack.dll", nullptr,
                                       LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                           LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return;
    // the chunk of the crate's test vectors, and the blocks the reference makes of it
    std::ifstream chunk_file("bc1.bcpack", std::ios::binary);
    std::ifstream expected_file("bc1.expected", std::ios::binary);
    REQUIRE(chunk_file);
    REQUIRE(expected_file);
    const std::vector<std::uint8_t> chunk{
        std::istreambuf_iterator<char>(chunk_file), std::istreambuf_iterator<char>()};
    const std::vector<std::uint8_t> expected{
        std::istreambuf_iterator<char>(expected_file), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(chunk.empty());
    REQUIRE(expected.size() % 8 == 0);
    Runtime runtime;
    auto *const queue = runtime.queue();
    auto *const status = runtime.status(4);
    std::vector<std::uint8_t> destination(expected.size(), 0);
    Request request{};
    request.options = option_source_memory | (1ull << 1); // BC1
    request.destination = destination.data();
    request.destination_size = static_cast<UINT32>(destination.size());
    request.source = const_cast<std::uint8_t *>(chunk.data());
    request.source_size = static_cast<UINT32>(chunk.size());
    queue->EnqueueRequest(&request);
    queue->EnqueueStatus(status, 0);
    queue->Submit();
    CHECK(status->GetHResult(0) == S_OK);
    CHECK(destination == expected);
    queue->Release();
    status->Release();
}
