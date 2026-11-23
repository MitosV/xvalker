#include "runtime/modules/acphal.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/memory.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <intrin.h>
#include <mutex>
#include <utility>
#include <vector>
#include <windows.h>

// The audio processor without SHAPE: commands are acknowledged and audio frames ticked so
// the title's mixer runs, nothing is decoded. ABI as FMOD Ex calls it.
namespace xv::runtime::modules {
namespace {
std::atomic<bool> early_kit{false};
struct ApuRegion {
    std::uintptr_t address;
    std::uint32_t size, apu;
};
std::mutex apu_mutex;
std::vector<ApuRegion> apu_regions;
std::uint32_t next_apu = 0x10000000;
std::uint32_t apu_address(std::uintptr_t address, std::uint32_t size) {
    for (const auto &region : apu_regions)
        if (address - region.address < region.size)
            return region.apu + static_cast<std::uint32_t>(address - region.address);
    const auto apu = next_apu;
    next_apu += (std::max(size, 1u) + 0xfff) & ~0xfffu;
    apu_regions.push_back({address, size, apu});
    return apu;
}
void *apu_memory(std::size_t size, std::uint64_t alignment) {
    auto *const address = allocate_aligned(size, alignment, nullptr);
    if (address) {
        const std::lock_guard lock(apu_mutex);
        apu_address(reinterpret_cast<std::uintptr_t>(address), static_cast<std::uint32_t>(size));
    }
    return address;
}
void release_apu_memory(void *address) {
    {
        const std::lock_guard lock(apu_mutex);
        std::erase_if(apu_regions, [&](const ApuRegion &region) {
            return region.address == reinterpret_cast<std::uintptr_t>(address);
        });
    }
    VirtualFree(address, 0, MEM_RELEASE);
}
HRESULT WINAPI apu_alloc(void **address, std::uint32_t *apu, std::uint32_t size,
                         std::uint32_t alignment, std::uint32_t) {
    if (!address)
        return E_INVALIDARG;
    *address = apu_memory(size, alignment);
    if (!*address)
        return E_OUTOFMEMORY;
    if (apu) {
        const std::lock_guard lock(apu_mutex);
        *apu = apu_address(reinterpret_cast<std::uintptr_t>(*address), size);
    }
    return S_OK;
}
HRESULT WINAPI apu_free(void *address) {
    if (!address)
        return E_INVALIDARG;
    release_apu_memory(address);
    return S_OK;
}
HRESULT WINAPI apu_map_virtual_address(std::uint32_t *apu, void *address, std::uint32_t size) {
    if (!apu)
        return E_INVALIDARG;
    const std::lock_guard lock(apu_mutex);
    *apu = apu_address(reinterpret_cast<std::uintptr_t>(address), size);
    return S_OK;
}
// ApuMapApuAddress: the memory an address of the audio processor's stands for
void *WINAPI apu_map_apu_address(std::uint32_t apu) {
    const std::lock_guard lock(apu_mutex);
    for (const auto &region : apu_regions)
        if (apu - region.apu < region.size)
            return reinterpret_cast<void *>(region.address + (apu - region.apu));
    return nullptr;
}
BOOL WINAPI apu_is_virtual_address_valid() { return TRUE; }
// ApuHeapGetState: the heap is the host's memory, so there is always room. Free bytes, bytes
// in use, bytes lost, the largest block to be had, and the number of blocks.
HRESULT WINAPI apu_heap_state(std::uint32_t *state, std::uint32_t) {
    if (!state)
        return E_INVALIDARG;
    const std::lock_guard lock(apu_mutex);
    std::uint32_t used = 0;
    for (const auto &region : apu_regions)
        used += region.size;
    state[0] = 256u << 20;
    state[1] = used;
    state[2] = 0;
    state[3] = 64u << 20;
    state[4] = static_cast<std::uint32_t>(apu_regions.size());
    return S_OK;
}
// chad.dll (XDsp, the Series console's audio DSP): its interface is in no public header,
// so a title is told it is not there
HRESULT WINAPI no_audio_dsp() { return E_NOTIMPL; }
// IAudioStateMonitor, which says how loud the system lets the title be: always in full. A
// title takes the object without looking at the result of asking for it.
struct StateMonitor {
    const void *const *vtable;
    std::atomic<ULONG> references{1};
};
HRESULT STDMETHODCALLTYPE monitor_query(StateMonitor *self, const GUID *, void **object) {
    if (!object)
        return E_POINTER;
    ++self->references;
    *object = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE monitor_add_ref(StateMonitor *self) { return ++self->references; }
ULONG STDMETHODCALLTYPE monitor_release(StateMonitor *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
HRESULT STDMETHODCALLTYPE monitor_register(StateMonitor *, void *, void *, std::int64_t *token) {
    if (token)
        *token = 1;
    return S_OK;
}
void STDMETHODCALLTYPE monitor_unregister(StateMonitor *, std::int64_t) {}
// SoundLevel: 0 muted, 1 low, 2 full
int STDMETHODCALLTYPE monitor_sound_level(StateMonitor *) { return 2; }
const void *const monitor_vtable[] = {&monitor_query,    &monitor_add_ref,    &monitor_release,
                                      &monitor_register, &monitor_unregister, &monitor_sound_level};
HRESULT WINAPI create_state_monitor(int, int, StateMonitor **monitor) {
    if (!monitor)
        return E_POINTER;
    *monitor = new StateMonitor{monitor_vtable};
    return S_OK;
}
HRESULT WINAPI apu_create_heap() { return S_OK; }

// 0x60 bytes; the rest is unknown
struct ShapeContexts {
    std::uint32_t counts[5], reserved;
    void *contexts[5];
};
static_assert(sizeof(ShapeContexts) == 0x40);
constexpr std::size_t context_bytes = 64; // the largest entry seen
std::mutex shape_mutex;
void *shape_arrays[5] = {};
HRESULT WINAPI allocate_shape_contexts(ShapeContexts *contexts) {
    if (!contexts)
        return E_INVALIDARG;
    const std::lock_guard lock(shape_mutex);
    for (int i = 0; i < 5; ++i) {
        contexts->contexts[i] = nullptr;
        if (!contexts->counts[i])
            continue;
        if (!(shape_arrays[i] = apu_memory(contexts->counts[i] * context_bytes, 256)))
            return E_OUTOFMEMORY;
        contexts->contexts[i] = shape_arrays[i];
    }
    std::fprintf(stderr, "[Runtime] SHAPE contexts: %u, %u, %u, %u, %u (none of them run)\n",
                 contexts->counts[0], contexts->counts[1], contexts->counts[2], contexts->counts[3],
                 contexts->counts[4]);
    std::fflush(stderr);
    return S_OK;
}
HRESULT WINAPI release_shape_contexts() {
    const std::lock_guard lock(shape_mutex);
    for (auto *&array : shape_arrays)
        if (array)
            release_apu_memory(std::exchange(array, nullptr));
    return S_OK;
}

// A message as PopMessage gives it: its type, then what it is about. For a completed command
// that is the command's type, its id and the audio frame it completed in. The first
// development kits end after the id.
struct Message {
    std::uint32_t type, reserved;
    std::uint64_t value, id;
    std::uint32_t frame, unused;
};
static_assert(sizeof(Message) == 32);
constexpr std::size_t early_message_bytes = 24;
constexpr std::uint32_t audio_frame = 2, command_done = 0x10, disconnected = 0x80;
constexpr auto frame = std::chrono::microseconds(2667);

struct Hal;
struct HalVtable {
    HRESULT(STDMETHODCALLTYPE *connect)(Hal *, std::uint32_t, std::uint32_t);
    HRESULT(STDMETHODCALLTYPE *disconnect)(Hal *);
    HRESULT(STDMETHODCALLTYPE *submit)
    (Hal *, std::uint32_t, std::uint64_t, std::uint32_t, void *, std::uint32_t);
    bool(STDMETHODCALLTYPE *pop)(Hal *, Message *);
    std::uint32_t(STDMETHODCALLTYPE *message_count)(Hal *);
    void(STDMETHODCALLTYPE *release)(Hal *);
};
struct Hal {
    const HalVtable *vtable;
    std::mutex mutex;
    std::deque<Message> messages;
    bool connected = false;
    std::chrono::steady_clock::time_point next_frame;
    std::uint32_t seen[8] = {}; // command types logged
    std::uint64_t commands = 0; // completed so far
    std::uint32_t frames = 0;   // audio frames ticked so far
};
HRESULT STDMETHODCALLTYPE connect(Hal *self, std::uint32_t commands, std::uint32_t messages) {
    const std::lock_guard lock(self->mutex);
    self->connected = true;
    self->next_frame = std::chrono::steady_clock::now() + frame;
    std::fprintf(stderr, "[Runtime] audio processor connected (%u commands, %u messages)\n",
                 commands, messages);
    std::fflush(stderr);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE disconnect(Hal *self) {
    const std::lock_guard lock(self->mutex);
    self->connected = false;
    self->messages.push_back({disconnected, 0, 0, 0, 0, 0});
    return S_OK;
}
HRESULT STDMETHODCALLTYPE submit(Hal *self, std::uint32_t type, std::uint64_t id,
                                 std::uint32_t frame_index, void *data, std::uint32_t) {
    const std::lock_guard lock(self->mutex);
    if (!self->connected)
        return E_UNEXPECTED;
    if (type < 256 && !(self->seen[type / 32] & 1u << type % 32)) {
        self->seen[type / 32] |= 1u << type % 32;
        std::fprintf(stderr, "[Runtime] audio processor command 0x%x\n", type);
        std::fflush(stderr);
    }
    if (static std::atomic<int> shown{0}; std::getenv("XV_ACP_TRACE") && shown++ < 40) {
        const auto *words = static_cast<const std::uint32_t *>(data);
        std::fprintf(stderr,
                     "[Runtime] audio command %u id %llx frame %u data %x %x %x %x from %s\n", type,
                     static_cast<unsigned long long>(id), frame_index, words ? words[0] : 0,
                     words ? words[1] : 0, words ? words[2] : 0, words ? words[3] : 0,
                     detail::guest_location(_ReturnAddress()).c_str());
    }
    // Every command is completed. One the title gave no id is known by its place among the
    // commands, counted from 1: a title that sends its first command without an id waits for
    // the completion numbered 1 before it sends another.
    const auto sequence = ++self->commands;
    Message done{command_done, 0, type, id ? id : sequence, self->frames, 0};
    // the first development kits have the command's id right after its type, with no time
    // before them
    if (early_kit)
        done.value = std::uint64_t{static_cast<std::uint32_t>(done.id)} << 32 | type;
    self->messages.push_back(done);
    return S_OK;
}
bool STDMETHODCALLTYPE pop(Hal *self, Message *message) {
    if (!message)
        return false;
    if (static std::atomic<int> shown{0}; std::getenv("XV_ACP_TRACE") && shown++ < 1)
        std::fprintf(stderr, "[Runtime] PopMessage from %s\n",
                     detail::guest_location(_ReturnAddress()).c_str());
    const std::lock_guard lock(self->mutex);
    if (self->messages.empty() && self->connected) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= self->next_frame) {
            self->next_frame = std::max(self->next_frame + frame, now - 8 * frame);
            ++self->frames;
            const Message tick{audio_frame, 0, 0, 0, 0, 0};
            std::memcpy(message, &tick, early_kit ? early_message_bytes : sizeof tick);
            return true;
        }
    }
    if (self->messages.empty())
        return false;
    std::memcpy(message, &self->messages.front(),
                early_kit ? early_message_bytes : sizeof(Message));
    self->messages.pop_front();
    return true;
}
// GetNumMessages: what PopMessage has to give, the audio frame that is due among them
std::uint32_t STDMETHODCALLTYPE message_count(Hal *self) {
    const std::lock_guard lock(self->mutex);
    const bool frame_due = self->connected && std::chrono::steady_clock::now() >= self->next_frame;
    return static_cast<std::uint32_t>(self->messages.size()) + frame_due;
}
void STDMETHODCALLTYPE release(Hal *self) { delete self; }
constexpr HalVtable hal_vtable{&connect, &disconnect, &submit, &pop, &message_count, &release};

HRESULT WINAPI acp_hal_create(Hal **hal) {
    if (!hal)
        return E_INVALIDARG;
    *hal = new Hal{&hal_vtable};
    return S_OK;
}

// xapu.dll, the Series console's audio decoder: Opus decoding and sample rate conversion
// on the audio hardware, as a queue of commands and their results. Nothing is decoded here:
// every command is answered at once, a Process command with silence of the length asked for.
#pragma pack(push, 1)
struct XApuCommand {
    std::uint32_t type, stream, sequence, options;
    void *context;
};
struct XApuActivate {
    XApuCommand id;
    std::uint32_t channels;
};
struct XApuProcess {
    XApuCommand id;
    float ramp, pitch;
    std::uint32_t first_frame, frames, input_bytes;
    void *input;
    std::uint32_t output_bytes;
    void *output;
};
struct XApuResult {
    XApuCommand id;
    std::uint32_t state;
    std::uint64_t state_data, time;
    HRESULT error;
    std::uint32_t frames, output_bytes;
    void *output;
    float pitch;
    std::uint32_t input_bytes;
    void *input;
};
struct XApuConnectInput {
    std::uint32_t process, streams, commands_per_stream, data_bytes, frames_per_output, options;
};
struct XApuConnectOutput {
    void *data;
    HANDLE ready;
};
#pragma pack(pop)
struct XApu {
    std::mutex mutex;
    std::deque<XApuResult> results;
    std::vector<std::uint32_t> channels;
    void *data = nullptr;
    HANDLE ready = nullptr;
    std::uint32_t frames_per_output = 0;
};
constexpr HRESULT xapu_no_results = static_cast<HRESULT>(0x8ac81001);
HRESULT WINAPI xapu_connect(const XApuConnectInput *input, XApuConnectOutput *output,
                            XApu **handle) {
    if (!input || !output || !handle)
        return E_INVALIDARG;
    auto *const client = new XApu;
    client->channels.assign(std::max(input->streams, 1u), 2);
    client->frames_per_output = input->frames_per_output;
    if (input->data_bytes)
        client->data = allocate_aligned(input->data_bytes, 0x10000, nullptr);
    if (input->options & 8)
        client->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    *output = {client->data, client->ready};
    *handle = client;
    std::fprintf(stderr,
                 "[Runtime] audio decoder connected: process type %u, %u streams (nothing is "
                 "decoded: silence)\n",
                 input->process, input->streams);
    std::fflush(stderr);
    return S_OK;
}
HRESULT WINAPI xapu_disconnect(XApu *client) {
    if (!client)
        return E_INVALIDARG;
    if (client->data)
        VirtualFree(client->data, 0, MEM_RELEASE);
    if (client->ready)
        CloseHandle(client->ready);
    delete client;
    return S_OK;
}
HRESULT WINAPI xapu_enqueue(XApu *client, const XApuCommand *command, std::uint32_t *count) {
    if (!client || !command)
        return E_INVALIDARG;
    XApuResult result{};
    result.id = *command;
    result.pitch = 1;
    const std::lock_guard lock(client->mutex);
    const auto stream = command->stream < client->channels.size() ? command->stream : 0;
    if (command->type == 1) {
        const auto channels = reinterpret_cast<const XApuActivate *>(command)->channels;
        client->channels[stream] = channels == 1 ? 1 : 2;
    } else if (command->type == 3) {
        const auto *const process = reinterpret_cast<const XApuProcess *>(command);
        const std::uint32_t frame_bytes = client->channels[stream] * sizeof(float);
        std::uint32_t frames = process->frames ? process->frames : client->frames_per_output;
        frames = std::min(frames ? frames : 960, process->output_bytes / frame_bytes);
        if (process->output && frames)
            std::memset(process->output, 0, std::size_t{frames} * frame_bytes);
        result.state = 1 | 2; // output ready, and more input wanted
        result.frames = frames;
        result.output_bytes = frames * frame_bytes;
        result.output = process->output;
        result.pitch = process->pitch;
        result.input_bytes = process->input_bytes;
        result.input = process->input;
    }
    client->results.push_back(result);
    if (count)
        *count = static_cast<std::uint32_t>(client->results.size());
    if (client->ready && command->options & 1)
        SetEvent(client->ready);
    return S_OK;
}
HRESULT WINAPI xapu_dequeue(XApu *client, XApuResult *result) {
    if (!client || !result)
        return E_INVALIDARG;
    const std::lock_guard lock(client->mutex);
    if (client->results.empty())
        return xapu_no_results;
    *result = client->results.front();
    client->results.pop_front();
    return S_OK;
}
std::uint32_t WINAPI xapu_result_count(XApu *client, std::uint32_t *total) {
    const std::lock_guard lock(client->mutex);
    const auto count = static_cast<std::uint32_t>(client->results.size());
    if (total)
        *total = count;
    return count;
}
// XApuUtilization: streams available, and the most there can be
HRESULT WINAPI xapu_utilization(std::uint32_t, std::uint32_t, std::uint32_t *utilization) {
    if (!utilization)
        return E_INVALIDARG;
    utilization[0] = utilization[1] = 256;
    return S_OK;
}

template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
} // namespace

void install_acphal(Registry &registry) {
    const std::pair<const char *, void *> functions[] = {
        {"ApuAlloc", erase(apu_alloc)},
        {"ApuFree", erase(apu_free)},
        {"ApuCreateHeap", erase(apu_create_heap)},
        {"ApuMapVirtualAddress", erase(apu_map_virtual_address)},
        {"ApuMapApuAddress", erase(apu_map_apu_address)},
        {"ApuHeapGetState", erase(apu_heap_state)},
        {"ApuIsVirtualAddressValid", erase(apu_is_virtual_address_valid)},
        {"AcpHalCreate", erase(acp_hal_create)},
        {"AcpHalAllocateShapeContexts", erase(allocate_shape_contexts)},
        {"AcpHalReleaseShapeContexts", erase(release_shape_contexts)}};
    for (const auto &[name, function] : functions)
        registry.add("acphal.dll", name, function);
    const std::pair<const char *, void *> decoder[] = {
        {"XApuConnect", erase(xapu_connect)},
        {"XApuDisconnect", erase(xapu_disconnect)},
        {"XApuEnqueueCommand", erase(xapu_enqueue)},
        {"XApuDequeueResult", erase(xapu_dequeue)},
        {"XApuGetResultCount", erase(xapu_result_count)},
        {"XApuGetUtilization", erase(xapu_utilization)}};
    for (const auto &[name, function] : decoder)
        registry.add("xapu.dll", name, function);
    for (const auto *name :
         {"XDspConnect", "XDspConnectWithMaximumStreamLimit", "XDspDisconnect", "XDspActivate", "XDspDeactivate", "XDspSubmitCommand"})
        registry.add("chad.dll", name, erase(no_audio_dsp));
    registry.add("windows.media.mediacontrol.dll",
                 "CreateRenderAudioStateMonitorForCategoryAndDeviceRole",
                 erase(create_state_monitor));
}
void early_audio_processor(bool yes) { early_kit = yes; }
} // namespace xv::runtime::modules
