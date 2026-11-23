#include "runtime/modules/dstorage.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/bcpack.h"
#include "runtime/modules/inflate.hpp"
#include "runtime/modules/kernel.hpp"
#include "runtime/modules/memory.hpp"
#include "runtime/modules/placement.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <variant>
#include <vector>
#include <windows.h>

#include <d3d12.h>
#include <unknwn.h>

// DirectStorage as the Series console has it (dstorage_xs.dll, and XCRDAPI.dll for the
// factory): queues of read requests, with marks in between that set a status or signal a
// fence once everything before them are done. Here a queue does its work when it is
// submitted, on the thread that submits it, in order. Reads may come from a file, from
// memory or from the title's physical pages, and may be inflated (zlib) and decoded
// (BCPack, through bcpack.dll); the console's hardware texture swizzle is not known here,
// and such a request fails.
namespace xv::runtime::modules {
namespace {
constexpr GUID file_iid{
    0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr GUID factory_iid{
    0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};
constexpr GUID status_iid{
    0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID queue_iid{
    0xa3f6649a, 0x8c2c, 0x4b3a, {0x8b, 0x6e, 0x10, 0x18, 0x6a, 0x04, 0xb9, 0xbc}};
// Later kits name the factory and the queue by other IIDs (seen next to the status array's in
// a title that asks for no other); they are taken for the same interfaces, and a method past
// the ones known answers E_NOTIMPL with a log line.
constexpr GUID later_factory_iid{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID later_queue_iid{
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
bool same_interface(REFIID asked, const GUID &own) {
    if (asked == own)
        return true;
    const bool later = (own == factory_iid && asked == later_factory_iid) ||
                       (own == queue_iid && asked == later_queue_iid);
    if (later)
        if (static std::atomic<int> shown{0}; shown++ < 4) {
            std::fprintf(stderr, "[Runtime] DirectStorage: a later kit's IID for the %s\n",
                         own == factory_iid ? "factory" : "queue");
            std::fflush(stderr);
        }
    return later;
}

// The console's DSTORAGE_REQUEST_OPTIONS: bit 0 zlib, bits 1 to 4 BCPack, bits 5 to 20
// the swizzle mode, bit 21 the destination in physical pages, bit 22 the source in
// memory, bit 23 the source in physical pages
constexpr UINT64 option_zlib = 1ull << 0;
constexpr UINT64 option_bcpack = 0xfull << 1;
constexpr UINT64 option_swizzle = 0xffffull << 5;
constexpr UINT64 option_destination_pages = 1ull << 21;
constexpr UINT64 option_source_memory = 1ull << 22;
constexpr UINT64 option_source_pages = 1ull << 23;
constexpr UINT64 option_known = (1ull << 24) - 1;

// The console's own results
constexpr HRESULT E_DSTORAGE_END_OF_FILE = static_cast<HRESULT>(0x89240007u);
constexpr HRESULT E_DSTORAGE_ACCESS_VIOLATION = static_cast<HRESULT>(0x89240009u);
constexpr HRESULT E_DSTORAGE_RESERVED_FIELDS = static_cast<HRESULT>(0x8924000cu);
constexpr HRESULT E_DSTORAGE_INVALID_DESTINATION_SIZE = static_cast<HRESULT>(0x8924000fu);
constexpr HRESULT E_DSTORAGE_INVALID_FILE_HANDLE = static_cast<HRESULT>(0x89240017u);
constexpr HRESULT E_DSTORAGE_INVALID_SOURCE_TYPE = static_cast<HRESULT>(0x8924001bu);
constexpr HRESULT E_DSTORAGE_INVALID_INTERMEDIATE_SIZE = static_cast<HRESULT>(0x8924001cu);
constexpr HRESULT E_DSTORAGE_DECOMPRESSION_ERROR = static_cast<HRESULT>(0x89240030u);
constexpr HRESULT E_DSTORAGE_ZLIB_BAD_DATA = static_cast<HRESULT>(0x89240032u);

template <class Interface> class Object : public Interface {
  public:
    explicit Object(const GUID &iid) : iid_(iid) {}
    virtual ~Object() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid != IID_IUnknown && !same_interface(iid, iid_)) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        *object = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }

  private:
    GUID iid_;
    std::atomic<ULONG> references_{1};
};

// the interfaces, with their methods in the order the console's header has them. The
// methods this runtime does not know answer E_NOTIMPL: they are there so that a title
// that calls one does not read past the end of the vtable.
struct FileInterface : IUnknown {
    virtual void Close() = 0;
    virtual HANDLE GetHandle() = 0;
    virtual HRESULT Unknown5() { return E_NOTIMPL; }
    virtual HRESULT Unknown6() { return E_NOTIMPL; }
    virtual UINT32 GetId() = 0;
    virtual HRESULT Unknown8() { return E_NOTIMPL; }
    virtual HRESULT Unknown9() { return E_NOTIMPL; }
    virtual HRESULT Unknown10() { return E_NOTIMPL; }
    virtual HRESULT Unknown11() { return E_NOTIMPL; }
};
struct StatusInterface : IUnknown {
    virtual BOOL IsComplete(UINT32 index) = 0;
    virtual HRESULT GetHResult(UINT32 index) = 0;
    virtual HRESULT Unknown5() { return E_NOTIMPL; }
    virtual HRESULT Unknown6() { return E_NOTIMPL; }
    virtual HRESULT Unknown7() { return E_NOTIMPL; }
    virtual HRESULT Unknown8() { return E_NOTIMPL; }
    virtual HRESULT Unknown9() { return E_NOTIMPL; }
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
    UINT64 options; // bit 0 zlib, bits 1 to 4 BCPack, bit 21 destination in pages, bit 22
                    // source in memory, bit 23 source in pages
    void *destination;
    UINT64 destination_page_offset; // 16 bits of it, in a union as large as two pointers
    UINT32 destination_size;
    void *source; // the file, or the memory
    UINT64 file_offset;
    UINT32 source_size, intermediate_size;
    UINT64 tag;
    const char *name;
};
static_assert(offsetof(Request, destination_size) == 24 && offsetof(Request, source) == 32 &&
              offsetof(Request, source_size) == 48 && offsetof(Request, tag) == 56);
// DSTORAGE_ERROR_RECORD: the number of failures, then the first one: its result, the
// command that failed (a request), the request's options, its destination and size, and
// the file it read from or the memory it read from
struct ErrorRecord {
    UINT32 failures;
    UINT32 reserved;
    HRESULT first;
    UINT32 command; // 0xFFFFFFFF when there is none
    UINT64 options;
    UINT64 destination;
    UINT32 destination_size;
    WCHAR file[0x104];
    UINT8 reserved2[0xc];
    UINT64 memory_source;
    UINT8 reserved3[0x50];
};
static_assert(sizeof(ErrorRecord) == 0x290 && offsetof(ErrorRecord, first) == 8 &&
              offsetof(ErrorRecord, command) == 0xc && offsetof(ErrorRecord, options) == 0x10 &&
              offsetof(ErrorRecord, destination) == 0x18 &&
              offsetof(ErrorRecord, destination_size) == 0x20 &&
              offsetof(ErrorRecord, file) == 0x24 &&
              offsetof(ErrorRecord, memory_source) == 0x238);
struct QueueInterface : IUnknown {
    virtual void EnqueueRequest(const Request *request) = 0;
    virtual void EnqueueStatus(StatusInterface *status, UINT32 index) = 0;
    virtual void EnqueueSignal(ID3D12Fence *fence, UINT64 value) = 0;
    virtual void Submit() = 0;
    virtual void CancelRequestsWithTag(UINT64 mask, UINT64 value) = 0;
    virtual void Close() = 0;
    virtual HANDLE GetErrorEvent() = 0;
    virtual void RetrieveErrorRecord(ErrorRecord *record) = 0;
    virtual void Query(QueueInfo *info) = 0;
    virtual HRESULT Unknown12() { return E_NOTIMPL; }
    virtual HRESULT Unknown13() { return E_NOTIMPL; }
    virtual HRESULT Unknown14() { return E_NOTIMPL; }
    virtual HRESULT Unknown15() { return E_NOTIMPL; }
    virtual HRESULT Unknown16() { return E_NOTIMPL; }
    virtual HRESULT Unknown17() { return E_NOTIMPL; }
};
struct FactoryInterface : IUnknown {
    virtual HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) = 0;
    virtual HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) = 0;
    virtual HRESULT CreateStatusArray(UINT32 capacity, const char *name, REFIID iid,
                                      void **status) = 0;
    virtual HRESULT SetCpuAffinity(UINT64 affinity) = 0;
    virtual void SetDebugFlags(UINT32 flags) = 0;
    virtual HRESULT SetStagingBufferSize(UINT32 size) = 0;
    virtual HRESULT Unknown9() { return E_NOTIMPL; }
    virtual HRESULT Unknown10() { return E_NOTIMPL; }
    virtual HRESULT Unknown11() { return E_NOTIMPL; }
    virtual HRESULT Unknown12() { return E_NOTIMPL; }
    virtual HRESULT Unknown13() { return E_NOTIMPL; }
    virtual HRESULT Unknown14() { return E_NOTIMPL; }
    virtual HRESULT Unknown15() { return E_NOTIMPL; }
};

class File final : public Object<FileInterface> {
  public:
    explicit File(HANDLE handle, std::wstring path = {})
        : Object(file_iid), handle_(handle), path_(std::move(path)), id_(++counter()) {}
    ~File() override { Close(); }
    void Close() override {
        if (const auto handle = handle_.exchange(INVALID_HANDLE_VALUE);
            handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    HANDLE GetHandle() override { return handle_; }
    // the number the console gives a file
    UINT32 GetId() override { return id_; }
    // the path the title opened, for the error record
    const std::wstring &path() const { return path_; }
    // whether a pointer is one of our files: the title's requests name their source
    static bool is(const void *object) {
        if (!object ||
            !readable_memory(reinterpret_cast<std::uint64_t>(object), sizeof(void *)))
            return false;
        return *reinterpret_cast<void *const *>(object) == vtable();
    }

  private:
    // the vtable of our files, so that a source that is not one of them is refused
    static const void *vtable() {
        static const void *const vtable = [] {
            File *const probe = new File(INVALID_HANDLE_VALUE);
            const void *const address = *reinterpret_cast<void *const *>(probe);
            probe->Release();
            return address;
        }();
        return vtable;
    }
    static UINT32 &counter() {
        static UINT32 next = 0;
        return next;
    }
    std::atomic<HANDLE> handle_;
    std::wstring path_;
    UINT32 id_;
};

class Status final : public Object<StatusInterface> {
  public:
    explicit Status(UINT32 capacity) : Object(status_iid), entries_(capacity) {}
    BOOL IsComplete(UINT32 index) override {
        const std::lock_guard lock(mutex_);
        return index < entries_.size() && entries_[index].done ? TRUE : FALSE;
    }
    HRESULT GetHResult(UINT32 index) override {
        const std::lock_guard lock(mutex_);
        if (index >= entries_.size())
            return E_INVALIDARG;
        return entries_[index].done ? entries_[index].result : E_PENDING;
    }
    void set(UINT32 index, HRESULT result) {
        const std::lock_guard lock(mutex_);
        if (index < entries_.size())
            entries_[index] = {true, result};
    }
    void pending(UINT32 index) {
        const std::lock_guard lock(mutex_);
        if (index < entries_.size())
            entries_[index] = {};
    }

  private:
    struct Entry {
        bool done = false;
        HRESULT result = S_OK;
    };
    std::mutex mutex_;
    std::vector<Entry> entries_;
};

// The BCPack decoder, in bcpack.dll next to the executable. It is loaded when the first
// compressed request comes; without it such a request fails as it did before.
XvBcpackDecode bcpack_decoder() {
    static XvBcpackDecode decoder = [] {
        const auto module = LoadLibraryExW(L"bcpack.dll", nullptr,
                                           LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                               LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            if (static std::atomic<int> shown{0}; shown++ < 4)
                std::fprintf(stderr,
                             "[Runtime] DirectStorage: bcpack.dll is not next to the "
                             "executable: compressed textures are not decoded\n");
            return static_cast<XvBcpackDecode>(nullptr);
        }
        return reinterpret_cast<XvBcpackDecode>(
            GetProcAddress(module, "xv_bcpack_decode"));
    }();
    return decoder;
}

// The title's physical pages are 64 KiB each
constexpr ULONG_PTR page_size = 0x10000;

// The physical pages of a request, copied: the title's array is often on its stack
struct PageArray {
    std::vector<ULONG_PTR> pages;
    UINT32 offset = 0;
};
bool snapshot_pages(const void *array, UINT32 offset, UINT32 size, PageArray &out) {
    out.offset = offset;
    if (!size)
        return true;
    if (!array || offset >= page_size)
        return false;
    const auto count =
        (static_cast<std::uint64_t>(offset) + size + page_size - 1) / page_size;
    if (count > 65536 ||
        !readable_memory(reinterpret_cast<std::uint64_t>(array),
                         static_cast<std::size_t>(count) * sizeof(ULONG_PTR)))
        return false;
    const auto *const numbers = static_cast<const ULONG_PTR *>(array);
    out.pages.assign(numbers, numbers + count);
    return true;
}

// A request, as the queue runs it
struct Work {
    Request request;
    File *file = nullptr; // the file it reads, when it reads one
    PageArray destination_pages;
    PageArray source_pages;
    UINT64 sequence = 0; // the order it was enqueued in
};

// The bytes a request reads: from its file, from the title's memory or from its
// physical pages
HRESULT read_source(const Work &work, std::vector<std::uint8_t> &bytes) {
    const auto &request = work.request;
    bytes.resize(request.source_size);
    if (request.options & option_source_memory) {
        if (request.options & option_source_pages) {
            if (work.source_pages.pages.empty() ||
                !read_title_physical_pages(work.source_pages.pages.data(),
                                          work.source_pages.pages.size(),
                                          work.source_pages.offset, bytes.data(), bytes.size()))
                return E_DSTORAGE_ACCESS_VIOLATION;
            return S_OK;
        }
        if (!readable_memory(reinterpret_cast<std::uint64_t>(request.source), bytes.size()))
            return E_DSTORAGE_ACCESS_VIOLATION;
        if (!bytes.empty())
            std::memcpy(bytes.data(), request.source, bytes.size());
        return S_OK;
    }
    if (!work.file)
        return E_DSTORAGE_INVALID_FILE_HANDLE;
    const auto handle = work.file->GetHandle();
    if (handle == INVALID_HANDLE_VALUE)
        return E_DSTORAGE_INVALID_FILE_HANDLE;
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(request.file_offset);
    at.OffsetHigh = static_cast<DWORD>(request.file_offset >> 32);
    DWORD done = 0;
    if (!ReadFile(handle, bytes.data(), request.source_size, &done, &at))
        return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_READ_FAULT);
    if (done != request.source_size)
        return E_DSTORAGE_END_OF_FILE;
    return S_OK;
}

// Where a request's result goes: the title's memory, or its physical pages
HRESULT write_destination(const Work &work, const std::uint8_t *bytes, UINT32 size) {
    if (work.request.options & option_destination_pages) {
        if (work.destination_pages.pages.empty())
            return E_DSTORAGE_ACCESS_VIOLATION;
        if (!write_title_physical_pages(work.destination_pages.pages.data(),
                                        work.destination_pages.pages.size(),
                                        work.destination_pages.offset, bytes, size))
            return E_DSTORAGE_ACCESS_VIOLATION;
        return S_OK;
    }
    prepare_title_write(work.request.destination, size);
    if (size)
        std::memcpy(work.request.destination, bytes, size);
    return S_OK;
}

HRESULT read(const Work &work) {
    const auto &request = work.request;
    if (request.options & option_swizzle) {
        // the console's hardware texture swizzle: the mode says how the destination is
        // laid out, and this runtime does not know it
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] DirectStorage: a request with the swizzle mode %llu is "
                         "not done\n",
                         static_cast<unsigned long long>((request.options & option_swizzle) >> 5));
        return E_DSTORAGE_DECOMPRESSION_ERROR;
    }
    const bool inflated = request.options & option_zlib;
    const auto mode = static_cast<UINT32>((request.options & option_bcpack) >> 1);
    // a plain read goes straight from the file to the destination
    if (!inflated && !mode && !(request.options & option_source_memory) &&
        !(request.options & option_destination_pages)) {
        if (request.source_size > request.destination_size)
            return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        if (!work.file)
            return E_DSTORAGE_INVALID_FILE_HANDLE;
        const auto handle = work.file->GetHandle();
        if (handle == INVALID_HANDLE_VALUE)
            return E_DSTORAGE_INVALID_FILE_HANDLE;
        prepare_title_write(request.destination, request.source_size);
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(request.file_offset);
        at.OffsetHigh = static_cast<DWORD>(request.file_offset >> 32);
        DWORD done = 0;
        if (!ReadFile(handle, request.destination, request.source_size, &done, &at))
            return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_READ_FAULT);
        if (done != request.source_size)
            return E_DSTORAGE_END_OF_FILE;
        return S_OK;
    }
    std::vector<std::uint8_t> packed;
    if (const auto result = read_source(work, packed); FAILED(result))
        return result;
    if (inflated) {
        // the intermediate size is the size of the inflated stream; without it the
        // stream is the size of the destination
        const auto stage = request.intermediate_size ? request.intermediate_size
                                                     : request.destination_size;
        std::vector<std::uint8_t> plain;
        plain.reserve(stage);
        if (!inflate_zlib({packed.data(), packed.size()}, plain, stage))
            return E_DSTORAGE_ZLIB_BAD_DATA;
        packed = std::move(plain);
    }
    if (mode) {
        // the console's texture compression, in bcpack.dll
        const auto decoder = bcpack_decoder();
        if (!decoder)
            return E_NOTIMPL;
        // the decoder writes nothing when it fails, so the destination can be its own
        std::vector<std::uint8_t> blocks;
        std::uint8_t *target = nullptr;
        if (request.options & option_destination_pages) {
            blocks.resize(request.destination_size);
            target = blocks.data();
        } else {
            prepare_title_write(request.destination, request.destination_size);
            target = static_cast<std::uint8_t *>(request.destination);
        }
        const auto result = decoder(mode, packed.data(), packed.size(), target,
                                    request.destination_size);
        if (FAILED(result))
            return result;
        if (request.options & option_destination_pages)
            return write_destination(work, blocks.data(), request.destination_size);
        return S_OK;
    }
    if (packed.size() > request.destination_size)
        return E_DSTORAGE_INVALID_DESTINATION_SIZE;
    return write_destination(work, packed.data(), request.destination_size);
}

class Queue final : public Object<QueueInterface> {
  public:
    explicit Queue(const QueueDesc &desc)
        : Object(queue_iid), desc_(desc), name_(desc.name ? desc.name : ""),
          error_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        desc_.name = name_.c_str();
    }
    ~Queue() override { CloseHandle(error_); }
    void EnqueueRequest(const Request *request) override {
        if (!request)
            return;
        Work work;
        work.request = *request;
        // the console refuses a request that does not fit its queue, and counts it as a
        // failure: the status after it gets the error
        if (const auto refusal = refuse(work); FAILED(refusal)) {
            {
                const std::lock_guard lock(mutex_);
                fail(refusal, work);
                pending_.emplace_back(Refused{refusal});
            }
            submit_when_full();
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            // the file is held until the request is done
            if (work.file)
                work.file->AddRef();
            work.sequence = ++sequence_;
            pending_.emplace_back(std::move(work));
        }
        submit_when_full();
    }
    void EnqueueStatus(StatusInterface *status, UINT32 index) override {
        if (!status)
            return;
        status->AddRef();
        static_cast<Status *>(status)->pending(index);
        {
            const std::lock_guard lock(mutex_);
            pending_.emplace_back(Mark{static_cast<Status *>(status), index});
        }
        submit_when_full();
    }
    void EnqueueSignal(ID3D12Fence *fence, UINT64 value) override {
        if (!fence)
            return;
        fence->AddRef();
        {
            const std::lock_guard lock(mutex_);
            pending_.emplace_back(Signal{fence, value});
        }
        submit_when_full();
    }
    void Submit() override {
        std::vector<Entry> work;
        {
            const std::lock_guard lock(mutex_);
            work.swap(pending_);
        }
        // one submission at a time, so that marks follow the requests before them
        const std::lock_guard running(running_);
        HRESULT since_mark = S_OK;
        for (auto &entry : work) {
            if (auto *const request = std::get_if<Work>(&entry)) {
                if (cancelled(request->sequence, request->request.tag)) {
                    if (request->file)
                        request->file->Release();
                    continue;
                }
                const auto result = read(*request);
                // XV_FILE_TRACE: the first requests, and every one that fails
                if (static std::atomic<int> shown{0};
                    std::getenv("XV_FILE_TRACE") && (FAILED(result) || shown < 24) && shown++ < 200)
                    std::fprintf(stderr,
                                 "[Runtime] DirectStorage request: options 0x%llx, %u bytes at "
                                 "%llu to %u bytes (intermediate %u): 0x%08lx\n",
                                 request->request.options, request->request.source_size,
                                 request->request.file_offset, request->request.destination_size,
                                 request->request.intermediate_size,
                                 static_cast<unsigned long>(result));
                if (request->file)
                    request->file->Release();
                if (FAILED(result)) {
                    if (SUCCEEDED(since_mark))
                        since_mark = result;
                    const std::lock_guard lock(mutex_);
                    fail(result, *request);
                }
            } else if (auto *const refused = std::get_if<Refused>(&entry)) {
                if (SUCCEEDED(since_mark))
                    since_mark = refused->result;
            } else if (auto *const mark = std::get_if<Mark>(&entry)) {
                mark->status->set(mark->index, since_mark);
                mark->status->Release();
                since_mark = S_OK;
            } else if (auto *const signal = std::get_if<Signal>(&entry)) {
                signal->fence->Signal(signal->value);
                signal->fence->Release();
            }
        }
    }
    void CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        std::lock_guard lock(mutex_);
        // the requests that are waiting go away
        std::erase_if(pending_, [&](Entry &entry) {
            auto *const request = std::get_if<Work>(&entry);
            if (!request || (request->request.tag & mask) != value)
                return false;
            if (request->file)
                request->file->Release();
            return true;
        });
        // and the ones that are on their way are skipped when they run
        cancels_.push_back({mask, value, sequence_});
    }
    void Close() override { Submit(); }
    HANDLE GetErrorEvent() override { return error_; }
    void RetrieveErrorRecord(ErrorRecord *record) override {
        if (!record)
            return;
        const std::lock_guard lock(mutex_);
        *record = {};
        record->failures = failures_;
        record->first = first_failure_;
        if (first_failure_ == S_OK) {
            record->command = 0xffffffff; // no command failed
        } else {
            record->command = 0; // a request
            record->options = first_request_.options;
            record->destination = reinterpret_cast<std::uint64_t>(first_request_.destination);
            record->destination_size = first_request_.destination_size;
            if (first_request_.options & option_source_memory) {
                record->memory_source =
                    reinterpret_cast<std::uint64_t>(first_request_.source);
            } else if (!first_file_.empty()) {
                const auto count = std::min<std::size_t>(first_file_.size(), 0x103);
                std::copy_n(first_file_.begin(), count, record->file);
            }
        }
        failures_ = 0;
        first_failure_ = S_OK;
        first_request_ = {};
        first_file_.clear();
        ResetEvent(error_);
    }
    void Query(QueueInfo *info) override {
        if (!info)
            return;
        const std::lock_guard lock(mutex_);
        const auto used =
            static_cast<UINT16>(std::min<std::size_t>(pending_.size(), desc_.capacity));
        const auto until = std::max<std::size_t>(desc_.capacity / 2, 1);
        *info = {desc_, static_cast<UINT16>(desc_.capacity - used),
                 static_cast<UINT16>(until > used ? until - used : 1)};
    }

  private:
    struct Mark {
        Status *status;
        UINT32 index;
    };
    struct Signal {
        ID3D12Fence *fence;
        UINT64 value;
    };
    struct Refused {
        HRESULT result;
    };
    // a request that is skipped when its tag matches a cancellation
    struct Cancellation {
        UINT64 mask, value, until;
    };
    using Entry = std::variant<Work, Mark, Signal, Refused>;
    // What the console refuses, the way it refuses it: a queue of files takes file
    // sources and a queue of memory takes memory ones, and the sizes have to fit the
    // stages the request names
    HRESULT refuse(Work &work) const {
        const auto &request = work.request;
        const bool memory = desc_.source_type != 0;
        const bool source_memory = request.options & option_source_memory;
        if (source_memory != memory)
            return E_DSTORAGE_INVALID_SOURCE_TYPE;
        if (!request.destination || (source_memory && !request.source))
            return E_DSTORAGE_ACCESS_VIOLATION;
        if (request.options & ~option_known) {
            if (static std::atomic<bool> shown{false}; !shown.exchange(true))
                std::fprintf(stderr,
                             "[Runtime] DirectStorage: a request with the unknown option bits "
                             "0x%llx\n",
                             static_cast<unsigned long long>(request.options & ~option_known));
        }
        const bool inflated = request.options & option_zlib;
        const auto mode = static_cast<UINT32>((request.options & option_bcpack) >> 1);
        if (mode > 7)
            return E_DSTORAGE_DECOMPRESSION_ERROR;
        if (!source_memory) {
            // the source is a file of ours
            if (!File::is(request.source))
                return E_DSTORAGE_INVALID_FILE_HANDLE;
            work.file = static_cast<File *>(request.source);
        }
        // the pages of the request are copied: the title's array is often on its stack
        if (request.options & option_destination_pages) {
            if (!snapshot_pages(request.destination,
                                static_cast<UINT32>(request.destination_page_offset & 0xffff),
                                request.destination_size, work.destination_pages))
                return E_DSTORAGE_ACCESS_VIOLATION;
        }
        if ((request.options & option_source_pages) && !source_memory)
            return E_DSTORAGE_INVALID_SOURCE_TYPE;
        if (request.options & option_source_pages) {
            if (!snapshot_pages(request.source,
                                static_cast<UINT32>(request.file_offset & 0xffff),
                                request.source_size, work.source_pages))
                return E_DSTORAGE_ACCESS_VIOLATION;
        }
        if (!inflated && !mode) {
            if (request.destination_size != request.source_size)
                return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        } else if (inflated && !mode) {
            if (request.destination_size <= request.source_size)
                return E_DSTORAGE_INVALID_DESTINATION_SIZE;
            if (request.intermediate_size &&
                (request.intermediate_size <= request.source_size ||
                 request.intermediate_size > request.destination_size))
                return E_DSTORAGE_INVALID_INTERMEDIATE_SIZE;
        } else if (mode && !inflated) {
            if (request.intermediate_size)
                return E_DSTORAGE_INVALID_INTERMEDIATE_SIZE;
        } else if (!request.intermediate_size) {
            // zlib and BCPack: the intermediate size is the size of the inflated stream
            return E_DSTORAGE_INVALID_INTERMEDIATE_SIZE;
        }
        return S_OK;
    }
    // the failure and the request it happened in, for the error record
    void fail(HRESULT result, const Work &work) {
        if (!failures_++) {
            first_failure_ = result;
            first_request_ = work.request;
            if (work.file)
                first_file_ = work.file->path();
        }
        SetEvent(error_);
    }
    // whether a request is cancelled: the rules are the ones the title set, for the
    // requests that were enqueued before it set them
    bool cancelled(UINT64 sequence, UINT64 tag) {
        const std::lock_guard lock(mutex_);
        for (const auto &rule : cancels_)
            if (sequence <= rule.until && (tag & rule.mask) == rule.value)
                return true;
        return false;
    }
    // a queue submits itself when half of it is taken
    void submit_when_full() {
        bool full;
        {
            const std::lock_guard lock(mutex_);
            full = pending_.size() >= std::max<std::size_t>(desc_.capacity / 2, 1);
        }
        if (full)
            Submit();
    }
    QueueDesc desc_;
    std::string name_;
    HANDLE error_;
    std::mutex mutex_, running_;
    std::vector<Entry> pending_;
    std::vector<Cancellation> cancels_;
    UINT64 sequence_ = 0;
    UINT32 failures_ = 0;
    HRESULT first_failure_ = S_OK;
    Request first_request_{};
    std::wstring first_file_;
};

class Factory final : public Object<FactoryInterface> {
  public:
    Factory() : Object(factory_iid) {}
    HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) override {
        if (!desc || !queue)
            return E_POINTER;
        auto *const made = new Queue(*desc);
        const auto result = made->QueryInterface(iid, queue);
        made->Release();
        return result;
    }
    HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) override {
        if (!path || !file)
            return E_POINTER;
        *file = nullptr;
        const auto handle =
            CreateFileW(console_file_path(path).c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                        nullptr); // a title opens the root of its package as well
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (static std::atomic<int> shown{0}; shown++ < 6)
                std::fprintf(stderr,
                             "[Runtime] DirectStorage: cannot open %ls (as %ls): error %lu\n", path,
                             console_file_path(path).c_str(), error);
            return HRESULT_FROM_WIN32(error);
        }
        if (static std::atomic<int> shown{0}; std::getenv("XV_FILE_TRACE") && shown++ < 40)
            std::fprintf(stderr, "[Runtime] DirectStorage: opened %ls (%zu characters) from %s\n",
                         path, wcslen(path), detail::guest_location(_ReturnAddress()).c_str());
        auto *const made = new File(handle, path);
        const auto result = made->QueryInterface(iid, file);
        made->Release();
        return result;
    }
    HRESULT CreateStatusArray(UINT32 capacity, const char *, REFIID iid, void **status) override {
        if (!status)
            return E_POINTER;
        auto *const made = new Status(capacity);
        const auto result = made->QueryInterface(iid, status);
        made->Release();
        return result;
    }
    HRESULT SetCpuAffinity(UINT64) override { return S_OK; }
    void SetDebugFlags(UINT32) override {}
    HRESULT SetStagingBufferSize(UINT32) override { return S_OK; }
};

HRESULT WINAPI get_factory(const GUID *iid, void **factory) {
    if (!iid || !factory)
        return E_POINTER;
    static Factory *const one = new Factory;
    return one->QueryInterface(*iid, factory);
}
} // namespace

void install_dstorage(Registry &registry) {
    for (const auto *module : {"dstorage_xs.dll", "xcrdapi.dll"})
        registry.add(module, "DStorageGetFactory", reinterpret_cast<void *>(&get_factory));
}
} // namespace xv::runtime::modules
