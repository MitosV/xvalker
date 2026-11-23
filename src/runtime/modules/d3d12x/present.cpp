#include "backends/graphics/backend.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include "runtime/modules/watchdog.hpp"
#include "runtime/modules/window.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dxgi1_6.h>
#include <intrin.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// How a GDK title shows its frames. The console has no swap chain: the title renders into
// resources of its own and hands one to ID3D12CommandQueue::PresentX, and paces itself with
// the device's frame events. The host shows pictures through a swap chain, so there is one
// here, on the title surface, of the size and format of what the title presents: each
// PresentX copies the title's resource into its back buffer and presents that.
namespace xv::runtime::modules::d3d12x {
namespace {
// D3D12XBOX_PRESENT_PLANE_PARAMETERS
struct PlaneParameters {
    UINT64 token;
    UINT resource_count;
    ID3D12Resource **resources;
    D3D12_RECT *source_rectangles;
    void *placement;
    UINT color_space, scale_filter, extended_count;
    void *extended;
    UINT flags;
};
static_assert(sizeof(PlaneParameters) == 0x48);

struct Presenter {
    IDXGISwapChain3 *chain = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool fullscreen = false; // the display is the chain's
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
};
std::mutex mutex;
Presenter presenter;
std::atomic<std::uint64_t> presents{0};
// SetFrameIntervalX: the length of a frame, 60 Hz until the title says otherwise
std::atomic<UINT> interval_microseconds{16667};

// a format a flip swap chain takes, for what the title presents; UNKNOWN if there is none
DXGI_FORMAT chain_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
void release(Presenter &state) {
    // a chain that has the display cannot be let go of
    if (state.chain && state.fullscreen)
        state.chain->SetFullscreenState(FALSE, nullptr);
    for (IUnknown *object :
         {static_cast<IUnknown *>(state.chain), static_cast<IUnknown *>(state.list),
          static_cast<IUnknown *>(state.allocator), static_cast<IUnknown *>(state.fence)})
        if (object)
            object->Release();
    state = {};
}
void wait_for(ID3D12Fence *fence, UINT64 value) {
    if (fence->GetCompletedValue() >= value)
        return;
    auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(value, event);
    WaitForSingleObject(event, 5000);
    CloseHandle(event);
}
// the swap chain for pictures like `desc`, made again when the title changes what it presents
HRESULT prepare(ID3D12CommandQueue *queue, const D3D12_RESOURCE_DESC &desc) {
    const auto format = chain_format(desc.Format);
    const auto width = static_cast<UINT>(desc.Width);
    if (presenter.chain && presenter.width == width && presenter.height == desc.Height &&
        presenter.format == format)
        return S_OK;
    if (presenter.fence)
        wait_for(presenter.fence, presenter.value);
    release(presenter);
    ID3D12Device *device = nullptr;
    IDXGIFactory4 *factory = nullptr;
    IDXGISwapChain1 *chain = nullptr;
    auto result = queue->GetDevice(IID_PPV_ARGS(&device));
    if (SUCCEEDED(result))
        result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    auto *const window = static_cast<HWND>(title_window().native());
    DXGI_SWAP_CHAIN_DESC1 chain_desc{};
    chain_desc.Width = width;
    chain_desc.Height = desc.Height;
    chain_desc.Format = format;
    chain_desc.SampleDesc = {1, 0};
    chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain_desc.BufferCount = 2;
    chain_desc.Scaling = DXGI_SCALING_STRETCH;
    chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(result))
        result =
            factory->CreateSwapChainForHwnd(queue, window, &chain_desc, nullptr, nullptr, &chain);
    if (SUCCEEDED(result)) {
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        result = chain->QueryInterface(IID_PPV_ARGS(&presenter.chain));
    }
    if (SUCCEEDED(result))
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&presenter.allocator));
    if (SUCCEEDED(result))
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, presenter.allocator,
                                           nullptr, IID_PPV_ARGS(&presenter.list));
    if (SUCCEEDED(result))
        result = presenter.list->Close();
    if (SUCCEEDED(result))
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presenter.fence));
    for (IUnknown *object : {static_cast<IUnknown *>(chain), static_cast<IUnknown *>(factory),
                             static_cast<IUnknown *>(device)})
        if (object)
            object->Release();
    std::fprintf(stderr,
                 "[Runtime] PresentX: a swap chain %ux%u format %u for the title's format %u on "
                 "the title surface: 0x%08lx\n",
                 width, desc.Height, static_cast<unsigned>(format),
                 static_cast<unsigned>(desc.Format), static_cast<unsigned long>(result));
    std::fflush(stderr);
    if (FAILED(result)) {
        release(presenter);
        return result;
    }
    presenter.width = width;
    presenter.height = desc.Height;
    presenter.format = format;
    surface::offer_exclusive_fullscreen();
    return S_OK;
}
// Native full screen (F11 with Options > Graphics > Full screen: Native): the display is
// given to the chain, or taken back, when the window asks for the other of the two. A flip
// chain needs its buffers made again after either; nothing holds one between frames.
void match_fullscreen(Presenter &state) {
    const bool wanted = surface::exclusive_fullscreen_wanted();
    if (wanted == state.fullscreen)
        return;
    auto result = state.chain->SetFullscreenState(wanted, nullptr);
    const auto resized =
        state.chain->ResizeBuffers(0, state.width, state.height, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(result))
        result = resized;
    std::fprintf(stderr, "[Runtime] native full screen %s: 0x%08lx\n", wanted ? "on" : "off",
                 static_cast<unsigned long>(result));
    std::fflush(stderr);
    state.fullscreen = wanted && SUCCEEDED(result);
    if (wanted && FAILED(result)) {
        state.chain->SetFullscreenState(FALSE, nullptr);
        state.chain->ResizeBuffers(0, state.width, state.height, DXGI_FORMAT_UNKNOWN, 0);
        surface::exclusive_fullscreen_failed();
    }
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
// XV_D3D_PROBE=<frame>: what the title presents at that frame, and its render targets
void probe(ID3D12Resource *picture) {
    static const int at = [] {
        const auto *const text = std::getenv("XV_D3D_PROBE");
        const int frame = text ? std::atoi(text) : 0;
        return !text ? 0 : frame > 1 ? frame : 300;
    }();
    if (!at || presents.load() + 1 != static_cast<std::uint64_t>(at))
        return;
    std::vector<std::uint8_t> data;
    unsigned pitch = 0, width = 0, height = 0;
    DXGI_FORMAT format{};
    if (read_back(picture, data, pitch, width, height, format,
                  tracked_state(picture, D3D12_RESOURCE_STATE_PRESENT)))
        report_picture(
            data.data(), pitch, width, height, format,
            ("back buffer (" + std::to_string(draws_into_target(picture)) + " draws)").c_str());
    // XV_D3D_PROBE_TARGETS: the render targets as well. Not by default: a title may have let
    // go of one that is still listed, and reading that back takes the process down.
    int index = 0;
    if (std::getenv("XV_D3D_PROBE_TARGETS"))
        for (auto *const target : bound_render_targets())
            if (target != picture &&
                read_back(target, data, pitch, width, height, format,
                          tracked_state(target, D3D12_RESOURCE_STATE_RENDER_TARGET)))
                report_picture(data.data(), pitch, width, height, format,
                               ("render target " + std::to_string(index++) + " (" +
                                std::to_string(draws_into_target(target)) + " draws)")
                                   .c_str());
    report_failure();
}
} // namespace

// What became of each frame, by the token WaitFrameEventX gave for it: a title asks with
// GetFrameStatisticsX, and an engine holds its next frames until it is told that the ones
// before were shown. Times are the performance counter's.
struct FrameRecord {
    UINT64 token = 0;
    UINT64 wait_call = 0, wait_return = 0;
    UINT64 present_call = 0, present_return = 0;
    bool shown = false;
};
std::mutex record_mutex;
FrameRecord records[256];
std::atomic<bool> flipped{false}; // a frame was shown since the flip objects were signalled
UINT64 counter() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<UINT64>(value.QuadPart);
}
FrameRecord &record_of(UINT64 token) {
    auto &record = records[token % std::size(records)];
    if (record.token != token)
        record = {token};
    return record;
}
// the end of a PresentX, however it ends: the frame of its token counts as shown
struct Shown {
    UINT64 token, called = counter();
    ~Shown() {
        const std::lock_guard lock(record_mutex);
        auto &record = record_of(token);
        record.present_call = called;
        record.present_return = counter();
        record.shown = true;
        flipped = true;
    }
};

// XV_FRAME_PACE=1: frame events at the interval the title asked for, by the performance
// counter, and never before the host's fence of the frame before has completed.
// XV_SYNC_PRESENT=1: PresentX waits for the GPU to be done with the host's Present.
bool pace_enabled() {
    static const bool enabled = [] {
        const auto *const text = std::getenv("XV_FRAME_PACE");
        return text && *text == '1';
    }();
    return enabled;
}
bool sync_present_enabled() {
    static const bool enabled = [] {
        const auto *const text = std::getenv("XV_SYNC_PRESENT");
        return text && *text == '1';
    }();
    return enabled;
}
UINT64 counter_microseconds() {
    static const UINT64 frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<UINT64>(value.QuadPart);
    }();
    return counter() * 1000000 / frequency;
}
void sleep_until_microseconds(UINT64 target) {
    for (;;) {
        const auto now = counter_microseconds();
        if (now >= target)
            return;
        if (target - now > 2000)
            Sleep(static_cast<DWORD>((target - now - 1500) / 1000));
        else
            SwitchToThread();
    }
}
// the host's fence of the last frame (a reference of our own) and the value that frame signalled
std::mutex pace_mutex;
ID3D12Fence *pace_fence = nullptr;
UINT64 pace_value = 0;
void publish_pace_fence(ID3D12Fence *fence, UINT64 value) {
    const std::lock_guard lock(pace_mutex);
    if (pace_fence != fence) {
        if (pace_fence)
            pace_fence->Release();
        pace_fence = fence;
        fence->AddRef();
    }
    pace_value = value;
}
void wait_for_last_host_frame() {
    ID3D12Fence *fence;
    UINT64 value;
    {
        const std::lock_guard lock(pace_mutex);
        fence = pace_fence;
        value = pace_value;
        if (fence)
            fence->AddRef();
    }
    if (!fence)
        return;
    wait_for(fence, value);
    fence->Release();
}

// XV_FRAME_TRACE: how the title paces its frames, the first calls of each kind
bool frame_trace(std::atomic<int> &shown, int most = 12) {
    static const bool enabled = std::getenv("XV_FRAME_TRACE") != nullptr;
    return enabled && shown++ < most;
}

HRESULT present_x(ID3D12CommandQueue *queue, UINT planes, const void *plane_parameters,
                  const void *present_parameters) {
    if (static std::atomic<int> shown{0}; frame_trace(shown, 30)) {
        const auto *const first = static_cast<const PlaneParameters *>(plane_parameters);
        const auto *const whole = static_cast<const UINT *>(present_parameters);
        std::fprintf(
            stderr,
            "[Runtime] PresentX: %u planes, token %llu, %u resources, flags 0x%x, from %s\n",
            planes, first ? first->token : 0, first ? first->resource_count : 0,
            whole ? whole[6] : 0, detail::guest_location(_ReturnAddress()).c_str());
    }
    const Shown shown{
        plane_parameters ? static_cast<const PlaneParameters *>(plane_parameters)->token : 0};
    // no planes: the title shows nothing, which it does while it starts and suspends
    const auto *const plane = static_cast<const PlaneParameters *>(plane_parameters);
    if (!planes || !plane || !plane->resource_count || !plane->resources || !plane->resources[0])
        return S_OK;
    auto *const picture = plane->resources[0];
    const auto desc = picture->GetDesc();
    if (chain_format(desc.Format) == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count > 1) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] PresentX of a resource in format %u with %u samples, which "
                         "no swap chain of the host takes\n",
                         static_cast<unsigned>(desc.Format), desc.SampleDesc.Count);
        return S_OK;
    }
    const std::lock_guard lock(mutex);
    if (const auto result = prepare(queue, desc); FAILED(result))
        return result;
    probe(picture);
    auto &state = presenter;
    wait_for(state.fence, state.value);
    match_fullscreen(state);
    ID3D12Resource *back = nullptr;
    auto result =
        state.chain->GetBuffer(state.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    if (SUCCEEDED(result))
        result = state.allocator->Reset();
    if (SUCCEEDED(result))
        result = state.list->Reset(state.allocator, nullptr);
    if (SUCCEEDED(result)) {
        // whatever state the title left its picture in is where it stays
        const auto left = tracked_state(picture, D3D12_RESOURCE_STATE_PRESENT);
        D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(picture, left, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
        state.list->ResourceBarrier(2, barriers);
        state.list->CopyResource(back, picture);
        std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
        state.list->ResourceBarrier(2, barriers);
        result = state.list->Close();
    }
    if (SUCCEEDED(result)) {
        ID3D12CommandList *const lists[] = {state.list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(state.fence, ++state.value);
        // one display refresh per frame interval of the title's
        const UINT refreshes = std::clamp((interval_microseconds.load() + 8333) / 16667, 1u, 4u);
        result = state.chain->Present(refreshes, 0);
        if (sync_present_enabled()) {
            queue->Signal(state.fence, ++state.value);
            wait_for(state.fence, state.value);
        }
        if (pace_enabled())
            publish_pace_fence(state.fence, state.value);
    }
    if (back)
        back->Release();
    graphics::frame_counted();
    ++presents;
    frame_watchdog();
    title_window().show();
    // a hidden window does not wait for the display, and a title that sees a status stops
    if (result == DXGI_STATUS_OCCLUDED)
        Sleep(16);
    if (FAILED(result)) {
        if (static std::atomic<bool> reported{false}; !reported.exchange(true)) {
            std::fprintf(stderr, "[Runtime] PresentX failed on the host: 0x%08lx\n",
                         static_cast<unsigned long>(result));
            report_failure();
        }
        return result;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE set_frame_interval(ID3D12Device *, void *, UINT microseconds, UINT period,
                                             UINT flags) {
    if (static std::atomic<int> shown{0}; frame_trace(shown))
        std::fprintf(stderr,
                     "[Runtime] SetFrameIntervalX: %u microseconds, period %u, flags 0x%x\n",
                     microseconds, period, flags);
    if (microseconds)
        interval_microseconds = microseconds;
    return S_OK;
}
// ScheduleFrameEventX: the objects a title wants signalled at a point of every frame (its
// start, the vertical blank or the flip), each at an offset from it. A thread of ours keeps
// the frame's time and signals them; one schedule per kind of event, replaced by the next.
struct FrameObjects {
    UINT count;
    HANDLE *objects;
};
struct Schedule {
    std::vector<HANDLE> objects; // duplicates of the title's, so that they outlive its own
    UINT offset_microseconds = 0;
    bool enabled = false;
};
std::mutex schedule_mutex;
Schedule schedules[3];
void run_schedules() {
    using Clock = std::chrono::steady_clock;
    auto origin = Clock::now();
    for (;;) {
        const auto frame = std::chrono::microseconds(std::max(interval_microseconds.load(), 1000u));
        // each schedule at its offset, in the order of their offsets
        std::vector<std::pair<UINT, std::vector<HANDLE>>> due;
        {
            const std::lock_guard lock(schedule_mutex);
            const bool shown = flipped.exchange(false);
            for (std::size_t type = 0; type < std::size(schedules); ++type) {
                const auto &schedule = schedules[type];
                // the flip's objects only when a frame was shown since the last time
                if (schedule.enabled && !schedule.objects.empty() && (type != 2 || shown))
                    due.emplace_back(schedule.offset_microseconds, schedule.objects);
            }
        }
        std::stable_sort(due.begin(), due.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[offset, objects] : due) {
            std::this_thread::sleep_until(origin + std::min<std::chrono::microseconds>(
                                                       std::chrono::microseconds(offset), frame));
            for (const auto object : objects)
                if (!SetEvent(object))
                    ReleaseSemaphore(object, 1, nullptr);
        }
        origin += frame;
        if (origin < Clock::now() - frame * 4)
            origin = Clock::now(); // fell behind (a debugger, a stall): no burst to catch up
        std::this_thread::sleep_until(origin);
    }
}
// run_schedules for XV_FRAME_PACE: the same schedules on the performance counter, each signal
// held back until the host's fence of the last frame has completed
void run_paced_schedules() {
    auto origin = counter_microseconds();
    for (;;) {
        const UINT64 frame = std::max(interval_microseconds.load(), 1000u);
        std::vector<std::pair<UINT, std::vector<HANDLE>>> due;
        {
            const std::lock_guard lock(schedule_mutex);
            const bool shown = flipped.exchange(false);
            for (std::size_t type = 0; type < std::size(schedules); ++type) {
                const auto &schedule = schedules[type];
                if (schedule.enabled && !schedule.objects.empty() && (type != 2 || shown))
                    due.emplace_back(schedule.offset_microseconds, schedule.objects);
            }
        }
        std::stable_sort(due.begin(), due.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[offset, objects] : due) {
            sleep_until_microseconds(origin + std::min<UINT64>(offset, frame));
            wait_for_last_host_frame();
            for (const auto object : objects)
                if (!SetEvent(object))
                    ReleaseSemaphore(object, 1, nullptr);
        }
        origin += frame;
        if (origin + frame * 4 < counter_microseconds())
            origin = counter_microseconds(); // fell behind: no burst to catch up
        sleep_until_microseconds(origin);
    }
}
HRESULT STDMETHODCALLTYPE schedule_frame_event(ID3D12Device *, UINT type, UINT offset,
                                               void *signal_list, UINT flags) {
    const auto *const list = static_cast<const FrameObjects *>(signal_list);
    if (static std::atomic<int> shown{0}; frame_trace(shown))
        std::fprintf(
            stderr,
            "[Runtime] ScheduleFrameEventX: type %u, offset %u, %u objects, flags 0x%x, from %s\n",
            type, offset, list ? list->count : 0, flags,
            detail::guest_location(_ReturnAddress()).c_str());
    if (type >= std::size(schedules))
        return E_INVALIDARG;
    std::vector<HANDLE> objects;
    for (UINT i = 0; list && list->objects && i < list->count; ++i) {
        HANDLE copy = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), list->objects[i], GetCurrentProcess(), &copy, 0,
                            FALSE, DUPLICATE_SAME_ACCESS))
            objects.push_back(copy);
    }
    {
        const std::lock_guard lock(schedule_mutex);
        auto &schedule = schedules[type];
        if (list) {
            for (const auto old : schedule.objects)
                CloseHandle(old);
            schedule.objects = std::move(objects);
        }
        schedule.offset_microseconds = offset;
        schedule.enabled = !(flags & 1); // DISABLE_SIGNAL
    }
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread(pace_enabled() ? run_paced_schedules : run_schedules).detach();
    });
    return S_OK;
}
// The start of the next frame. A title that presents is held to the display's rate by the
// swap chain; one that waits without presenting in between is held here.
HRESULT STDMETHODCALLTYPE wait_frame_event(ID3D12Device *, UINT type, UINT timeout, void *list,
                                           UINT flags, UINT64 *token) {
    if (static std::atomic<int> shown{0}; frame_trace(shown))
        std::fprintf(
            stderr,
            "[Runtime] WaitFrameEventX: type %u, timeout %u, list %p, flags 0x%x, from %s\n", type,
            timeout, list, flags, detail::guest_location(_ReturnAddress()).c_str());
    using Clock = std::chrono::steady_clock;
    static std::mutex wait_mutex;
    static Clock::time_point last{};
    static std::uint64_t presents_seen = ~std::uint64_t{0};
    static UINT64 next_token = 0;
    const std::lock_guard lock(wait_mutex);
    const auto called = counter();
    const auto frame = std::chrono::microseconds(interval_microseconds.load());
    if (pace_enabled()) {
        // XV_FRAME_PACE: one frame event per interval of the title's, whether it presented or not
        static UINT64 target = 0;
        const UINT64 length = interval_microseconds.load();
        const UINT64 now = counter_microseconds();
        target = !target || now > target + length * 4 ? now : target + length;
        sleep_until_microseconds(target);
        wait_for_last_host_frame();
    } else if (presents_seen == presents.load() && Clock::now() < last + frame)
        std::this_thread::sleep_until(last + frame);
    last = Clock::now();
    presents_seen = presents.load();
    const auto given = ++next_token;
    {
        const std::lock_guard records_lock(record_mutex);
        auto &record = record_of(given);
        record.wait_call = called;
        record.wait_return = counter();
    }
    if (token)
        *token = given;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE frame_statistics(ID3D12Device *, UINT64 token, UINT types, UINT *count,
                                           void *statistics) {
    if (static std::atomic<int> shown{0}; frame_trace(shown))
        std::fprintf(
            stderr, "[Runtime] GetFrameStatisticsX: token %llu, types 0x%x, room for %u, from %s\n",
            token, types, count ? *count : 0, detail::guest_location(_ReturnAddress()).c_str());
    // D3D12XBOX_FRAME_STATISTICS: a type and 48 bytes of what goes with it; one entry for
    // each type asked for that there is something to say about
    struct Entry {
        UINT type;
        UINT padding;
        std::uint8_t data[48];
    };
    static_assert(sizeof(Entry) == 56);
    if (!count)
        return E_POINTER;
    const UINT room = statistics ? *count : 0;
    *count = 0;
    const std::lock_guard lock(record_mutex);
    const auto &record = records[token % std::size(records)];
    if (record.token != token)
        return S_OK;
    auto *const entries = static_cast<Entry *>(statistics);
    const auto put = [](Entry &entry, std::size_t at, auto value) {
        std::memcpy(entry.data + at, &value, sizeof value);
    };
    for (const UINT type : {0x1u, 0x2u, 0x100u, 0x1000u, 0x10000u}) {
        if (!(types & type) || *count >= room)
            continue;
        if (type >= 0x100 && !record.shown)
            continue; // not rendered, presented or displayed yet
        Entry entry{type, 0, {}};
        if (type == 0x1) { // the frame interval: its length, period and flags
            put(entry, 0, static_cast<UINT32>(interval_microseconds.load()));
            put(entry, 4, UINT32{1});
        } else if (type == 0x2) { // the frame event: when it was waited for and came
            put(entry, 8, record.wait_call);
            put(entry, 16, record.wait_return);
            put(entry, 24, record.wait_return);
        } else if (type == 0x100) { // rendering: when the GPU was done, and how long it took
            put(entry, 8, record.present_return);
            put(entry, 16, record.present_return - record.present_call);
        } else if (type == 0x1000) { // presenting: one plane, one view, nothing queued
            entry.data[1] = 1;
            entry.data[2] = 1;
            put(entry, 8, record.present_call);
            put(entry, 16, record.present_call);
            put(entry, 24, record.present_return);
            put(entry, 32, record.present_return - record.present_call);
            put(entry, 42, UINT16{1});
        } else { // the display: the vertical blank and the flip, at the top of the screen
            put(entry, 8, record.present_return);
            put(entry, 16, record.present_return);
        }
        entries[(*count)++] = entry;
    }
    return S_OK;
}

void report_picture(const std::uint8_t *data, unsigned pitch, unsigned width, unsigned height,
                    DXGI_FORMAT format, const char *what) {
    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool ten_bits =
        format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    // one pixel as blue, green, red, alpha
    const auto pixel = [&](UINT x, UINT y, std::uint8_t *out) {
        const auto *in = data + std::size_t{y} * pitch + std::size_t{x} * 4;
        if (ten_bits) {
            std::uint32_t value;
            std::memcpy(&value, in, 4);
            out[2] = static_cast<std::uint8_t>((value & 0x3ff) >> 2);
            out[1] = static_cast<std::uint8_t>(((value >> 10) & 0x3ff) >> 2);
            out[0] = static_cast<std::uint8_t>(((value >> 20) & 0x3ff) >> 2);
            out[3] = static_cast<std::uint8_t>((value >> 30) * 85);
        } else {
            std::memcpy(out, in, 4);
            if (rgba)
                std::swap(out[0], out[2]);
        }
    };
    double sum[4]{};
    std::uint8_t bgra[4];
    for (UINT y = 0; y < height; y += 8)
        for (UINT x = 0; x < width; x += 8) {
            pixel(x, y, bgra);
            sum[0] += bgra[2], sum[1] += bgra[1], sum[2] += bgra[0], sum[3] += bgra[3];
        }
    if (const auto *folder = std::getenv("TEMP"); folder && format != DXGI_FORMAT_D32_FLOAT)
        if (auto *file =
                std::fopen((std::string(folder) + "\\xv_" + what + ".bmp").c_str(), "wb")) {
            const std::uint32_t pixels = width * height * 4;
            std::uint8_t header[54]{'B', 'M'};
            const std::uint32_t size = 54 + pixels, offset = 54, info = 40,
                                planes_bits = 1 | (32 << 16);
            const std::int32_t signed_width = width,
                               signed_height = -static_cast<std::int32_t>(height);
            std::memcpy(header + 2, &size, 4);
            std::memcpy(header + 10, &offset, 4);
            std::memcpy(header + 14, &info, 4);
            std::memcpy(header + 18, &signed_width, 4);
            std::memcpy(header + 22, &signed_height, 4);
            std::memcpy(header + 26, &planes_bits, 4);
            std::fwrite(header, 1, sizeof header, file);
            std::vector<std::uint8_t> row(std::size_t{width} * 4);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x)
                    pixel(x, y, row.data() + std::size_t{x} * 4);
                std::fwrite(row.data(), 1, row.size(), file);
            }
            std::fclose(file);
        }
    const double count = (width / 8.0) * (height / 8.0);
    std::fprintf(stderr, "[Runtime] %s %ux%u format %u average %.1f %.1f %.1f %.1f\n", what, width,
                 height, static_cast<unsigned>(format), sum[0] / count, sum[1] / count,
                 sum[2] / count, sum[3] / count);
}
} // namespace xv::runtime::modules::d3d12x
