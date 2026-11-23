#include "runtime/common/com_forward.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/d3d11x/wrap.hpp"
#include "runtime/durango/runtime.hpp"

#include <atomic>
#include <audioclient.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <d3d11.h>
#include <ksmedia.h>
#include <mfobjects.h>
#include <mmdeviceapi.h>
#include <string>
#include <string_view>
#include <thread>
#include <unknwn.h>
#include <windows.h>

// Console-only functions. Undocumented ones only return.
namespace xv::runtime::durango {
namespace {
class Activation final : public IActivateAudioInterfaceAsyncOperation {
  public:
    Activation(HRESULT result, IUnknown *audio) : result_(result), audio_(audio) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IActivateAudioInterfaceAsyncOperation) &&
            iid != __uuidof(IAgileObject))
            return E_NOINTERFACE;
        *object = this;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left) {
            if (audio_)
                audio_->Release();
            delete this;
        }
        return left;
    }
    void finish(HRESULT result, IUnknown *audio) {
        result_ = result;
        audio_ = audio;
    }
    STDMETHODIMP GetActivateResult(HRESULT *result, IUnknown **audio) override {
        *result = result_;
        *audio = audio_;
        if (audio_)
            audio_->AddRef();
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
    HRESULT result_;
    IUnknown *audio_;
};
// The console mixes any PCM format; the host does in shared mode when asked to convert
HRESULT STDMETHODCALLTYPE initialize(Forwarder *self, AUDCLNT_SHAREMODE mode, DWORD flags,
                                     REFERENCE_TIME duration, REFERENCE_TIME period,
                                     const WAVEFORMATEX *format, const GUID *session) {
    auto *const client = static_cast<IAudioClient *>(forwarder_host(self));
    if (mode == AUDCLNT_SHAREMODE_SHARED)
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const auto result = client->Initialize(mode, flags, duration, period, format, session);
    std::fprintf(stderr,
                 "[Runtime] audio client: %u channels, %lu Hz, %u bits, flags 0x%lx: 0x%08lx\n",
                 format ? format->nChannels : 0, format ? format->nSamplesPerSec : 0,
                 format ? format->wBitsPerSample : 0, flags, static_cast<unsigned long>(result));
    return result;
}
HRESULT STDMETHODCALLTYPE format_supported(Forwarder *, AUDCLNT_SHAREMODE mode,
                                           const WAVEFORMATEX *, WAVEFORMATEX **closest) {
    if (closest)
        *closest = nullptr;
    return mode == AUDCLNT_SHAREMODE_SHARED ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
}
// the console always mixes 7.1 in float; titles check for eight channels
HRESULT STDMETHODCALLTYPE mix_format(Forwarder *self, WAVEFORMATEX **format) {
    if (!format)
        return E_POINTER;
    WAVEFORMATEX *host = nullptr;
    const auto result = static_cast<IAudioClient *>(forwarder_host(self))->GetMixFormat(&host);
    if (FAILED(result))
        return result;
    auto *const mix =
        static_cast<WAVEFORMATEXTENSIBLE *>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    *mix = {};
    mix->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix->Format.nChannels = 8;
    mix->Format.nSamplesPerSec = host->nSamplesPerSec;
    mix->Format.wBitsPerSample = 32;
    mix->Format.nBlockAlign = 8 * 4;
    mix->Format.nAvgBytesPerSec = mix->Format.nSamplesPerSec * mix->Format.nBlockAlign;
    mix->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    mix->Samples.wValidBitsPerSample = 32;
    mix->dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    mix->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    CoTaskMemFree(host);
    *format = &mix->Format;
    return S_OK;
}
// the console's AudioClientProperties are not the desktop's
HRESULT STDMETHODCALLTYPE client_properties(Forwarder *, const void *) { return S_OK; }
HRESULT audio_client_query(Forwarder *self, REFIID iid, void **object) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClient) ||
        iid == __uuidof(IAudioClient2) || iid == __uuidof(IAudioClient3)) {
        forwarder_add_ref(self);
        *object = self;
        return S_OK;
    }
    return forwarder_host(self)->QueryInterface(iid, object);
}
IUnknown *audio_client(IUnknown *host) {
    IAudioClient3 *client = nullptr;
    if (!host || FAILED(host->QueryInterface(IID_PPV_ARGS(&client))))
        return host;
    host->Release();
    return reinterpret_cast<IUnknown *>(
        make_forwarder(client, 21, &audio_client_query,
                       {{3, reinterpret_cast<void *>(&initialize)},
                        {7, reinterpret_cast<void *>(&format_supported)},
                        {8, reinterpret_cast<void *>(&mix_format)},
                        {16, reinterpret_cast<void *>(&client_properties)}}));
}
// "\\?\SWD#MMDEVAPI#{endpoint id}#{interface class}": the endpoint, or the default output
IMMDevice *endpoint(IMMDeviceEnumerator *devices, const wchar_t *path) {
    IMMDevice *device = nullptr;
    const std::wstring_view text = path ? path : L"";
    constexpr std::wstring_view marker = L"MMDEVAPI#";
    if (const auto at = text.find(marker); at != text.npos) {
        const auto start = at + marker.size(), end = text.find(L'#', start);
        const std::wstring id(text.substr(start, end - start));
        if (SUCCEEDED(devices->GetDevice(id.c_str(), &device)))
            return device;
    }
    devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    return device;
}
HRESULT WINAPI activate_audio_interface(const wchar_t *path, const GUID &iid, PROPVARIANT *,
                                        IActivateAudioInterfaceCompletionHandler *handler,
                                        IActivateAudioInterfaceAsyncOperation **operation) {
    if (!handler || !operation)
        return E_INVALIDARG;
    handler->AddRef();
    // the title may read the result from the operation it got back here
    auto *const shared = new Activation(E_PENDING, nullptr);
    shared->AddRef();
    std::thread([path = std::wstring(path ? path : L""), iid, handler, shared] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator *devices = nullptr;
        IUnknown *audio = nullptr;
        auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&devices));
        if (SUCCEEDED(result)) {
            auto *const device = endpoint(devices, path.c_str());
            result = device ? device->Activate(iid, CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void **>(&audio))
                            : E_NOTFOUND;
            if (device)
                device->Release();
            devices->Release();
        }
        wchar_t name[40];
        StringFromGUID2(iid, name, 40);
        std::fprintf(stderr, "[Runtime] audio interface %ls for %ls: 0x%08lx\n", name, path.c_str(),
                     static_cast<unsigned long>(result));
        if (SUCCEEDED(result) && (iid == __uuidof(IAudioClient) || iid == __uuidof(IAudioClient2) ||
                                  iid == __uuidof(IAudioClient3)))
            audio = audio_client(audio);
        shared->finish(result, audio);
        handler->ActivateCompleted(shared);
        shared->Release();
        handler->Release();
        CoUninitialize();
    }).detach();
    *operation = shared;
    return S_OK;
}
DWORD_PTR WINAPI set_wasapi_thread_affinity_mask(DWORD_PTR mask) { return mask; }
HRESULT WINAPI enable_spatial_audio() { return S_OK; }
void WINAPI nothing() {}

HRESULT WINAPI succeeded() { return S_OK; }
HRESULT WINAPI not_implemented() { return E_NOTIMPL; }

HRESULT WINAPI configure_counters(UINT, UINT, UINT, UINT) { return S_OK; }
UINT64 WINAPI zero() { return 0; }
void WINAPI report_counter(const wchar_t *, float) {}

// module+offset; there are no symbols
using SymbolCallback = BOOL (*)(void *, ULONG_PTR, HRESULT, const wchar_t *, ULONG);
HRESULT WINAPI get_symbol_from_address(int, DWORD count, ULONG_PTR *addresses,
                                       SymbolCallback callback, void *context) {
    if (!addresses || !callback)
        return E_INVALIDARG;
    for (DWORD i = 0; i < count; ++i) {
        const auto location = detail::guest_location(reinterpret_cast<void *>(addresses[i]));
        const auto plus = location.find('+');
        const std::wstring name(location.begin(),
                                location.begin() + (plus == std::string::npos ? 0 : plus));
        const auto offset =
            plus == std::string::npos ? 0 : std::strtoul(location.c_str() + plus + 3, nullptr, 16);
        if (!callback(context, addresses[i], name.empty() ? E_FAIL : S_OK, name.c_str(),
                      static_cast<ULONG>(offset)))
            break;
    }
    return S_OK;
}
BOOL WINAPI query_system_hardware_info() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

// MFCreateDXGIDeviceManager: the host's manager, whose decoders make their textures and
// views on the device the title gives it. They are the host's own code and get the host's
// device, not the object the title has for it.
HRESULT manager_query(Forwarder *self, REFIID iid, void **object) {
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFDXGIDeviceManager))
        return forwarder_host(self)->QueryInterface(iid, object);
    forwarder_add_ref(self);
    *object = self;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE reset_device(Forwarder *self, IUnknown *device, UINT token) {
    return static_cast<IMFDXGIDeviceManager *>(forwarder_host(self))
        ->ResetDevice(static_cast<IUnknown *>(d3d11x::unwrap(device)), token);
}
HRESULT WINAPI create_device_manager(UINT *token, IMFDXGIDeviceManager **manager) {
    using Create = HRESULT(WINAPI *)(UINT *, IMFDXGIDeviceManager **);
    static const auto create =
        reinterpret_cast<Create>(detail::host_export("mfplat.dll", "MFCreateDXGIDeviceManager"));
    if (!create || !manager)
        return E_NOTIMPL;
    IMFDXGIDeviceManager *host = nullptr;
    const auto result = create(token, &host);
    if (SUCCEEDED(result))
        *manager = reinterpret_cast<IMFDXGIDeviceManager *>(make_forwarder(
            host, 10, &manager_query, {{7, reinterpret_cast<void *>(&reset_device)}}));
    return result;
}
// MFResetDXGIDeviceManagerX(manager, device, token): the console's way to give the manager
// its device. Media Foundation decodes on threads of its own.
HRESULT WINAPI reset_device_manager(IMFDXGIDeviceManager *manager, IUnknown *device, UINT token) {
    if (!manager || !device)
        return E_INVALIDARG;
    if (ID3D10Multithread *threads = nullptr;
        SUCCEEDED(static_cast<IUnknown *>(d3d11x::unwrap(device))->QueryInterface(&threads))) {
        threads->SetMultithreadProtected(TRUE);
        threads->Release();
    }
    return manager->ResetDevice(device, token);
}

// MFCreateDxvaSampleRendererX(device, ?, renderer): an object with one method, by a title's
// calls of it: (context, sample, width, height) draws a decoded video frame over the render
// target bound at the time. The host's video processor does the conversion from the
// decoder's format.
template <class T> void drop(T *&object) {
    if (object)
        object->Release();
    object = nullptr;
}
struct SampleRenderer {
    void *const *vtable;
    std::atomic<ULONG> references{1};
    ID3D11VideoProcessorEnumerator *enumerator = nullptr;
    ID3D11VideoProcessor *processor = nullptr;
    UINT from_width = 0, from_height = 0, to_width = 0, to_height = 0;
};
HRESULT STDMETHODCALLTYPE renderer_query(SampleRenderer *self, REFIID, void **object) {
    ++self->references; // its one interface, whatever the title calls it
    *object = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE renderer_add_ref(SampleRenderer *self) { return ++self->references; }
ULONG STDMETHODCALLTYPE renderer_release(SampleRenderer *self) {
    const auto left = --self->references;
    if (!left) {
        drop(self->processor);
        drop(self->enumerator);
        delete self;
    }
    return left;
}
HRESULT STDMETHODCALLTYPE renderer_render(SampleRenderer *self, void *title_context,
                                          IUnknown *sample_or_buffer, UINT, UINT) {
    auto *const context = static_cast<ID3D11DeviceContext *>(d3d11x::unwrap(title_context));
    if (!context || !sample_or_buffer)
        return E_INVALIDARG;
    IMFMediaBuffer *buffer = nullptr;
    if (IMFSample *sample = nullptr; SUCCEEDED(sample_or_buffer->QueryInterface(&sample))) {
        sample->GetBufferByIndex(0, &buffer);
        sample->Release();
    } else {
        sample_or_buffer->QueryInterface(&buffer);
    }
    IMFDXGIBuffer *surface = nullptr;
    ID3D11Texture2D *frame = nullptr, *target = nullptr;
    ID3D11RenderTargetView *bound = nullptr;
    ID3D11Resource *resource = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11VideoDevice *video = nullptr;
    ID3D11VideoContext *video_context = nullptr;
    ID3D11VideoProcessorInputView *input = nullptr;
    ID3D11VideoProcessorOutputView *output = nullptr;
    UINT index = 0;
    auto result = E_FAIL;
    context->OMGetRenderTargets(1, &bound, nullptr);
    if (bound)
        bound->GetResource(&resource);
    if (buffer && SUCCEEDED(buffer->QueryInterface(&surface)) &&
        SUCCEEDED(surface->GetResource(IID_PPV_ARGS(&frame))) &&
        SUCCEEDED(surface->GetSubresourceIndex(&index)) && resource &&
        SUCCEEDED(resource->QueryInterface(&target))) {
        frame->GetDevice(&device);
        D3D11_TEXTURE2D_DESC from, to;
        frame->GetDesc(&from);
        target->GetDesc(&to);
        if (SUCCEEDED(device->QueryInterface(&video)) &&
            SUCCEEDED(context->QueryInterface(&video_context))) {
            if (!self->processor || self->from_width != from.Width ||
                self->from_height != from.Height || self->to_width != to.Width ||
                self->to_height != to.Height) {
                drop(self->processor);
                drop(self->enumerator);
                const D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{
                    D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
                    {30, 1},
                    from.Width,
                    from.Height,
                    {30, 1},
                    to.Width,
                    to.Height,
                    D3D11_VIDEO_USAGE_PLAYBACK_NORMAL};
                if (SUCCEEDED(video->CreateVideoProcessorEnumerator(&content, &self->enumerator)))
                    video->CreateVideoProcessor(self->enumerator, 0, &self->processor);
                self->from_width = from.Width, self->from_height = from.Height;
                self->to_width = to.Width, self->to_height = to.Height;
            }
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in{};
            in.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            in.Texture2D.ArraySlice = index;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out{};
            out.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (self->processor &&
                SUCCEEDED(
                    video->CreateVideoProcessorInputView(frame, self->enumerator, &in, &input)) &&
                SUCCEEDED(video->CreateVideoProcessorOutputView(target, self->enumerator, &out,
                                                                &output))) {
                D3D11_VIDEO_PROCESSOR_STREAM stream{};
                stream.Enable = TRUE;
                stream.pInputSurface = input;
                result = video_context->VideoProcessorBlt(self->processor, output, 0, 1, &stream);
            }
        }
    }
    if (static std::atomic<bool> told{false}; FAILED(result) && !told.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a video frame was not drawn: 0x%08lx\n",
                     static_cast<unsigned long>(result));
        std::fflush(stderr);
    }
    drop(output), drop(input), drop(video_context), drop(video), drop(device), drop(target);
    drop(resource), drop(bound), drop(frame), drop(surface), drop(buffer);
    return S_OK;
}
void *const renderer_vtable[] = {
    reinterpret_cast<void *>(&renderer_query), reinterpret_cast<void *>(&renderer_add_ref),
    reinterpret_cast<void *>(&renderer_release), reinterpret_cast<void *>(&renderer_render)};
HRESULT WINAPI create_sample_renderer(IUnknown *, void *, void **renderer) {
    if (!renderer)
        return E_POINTER;
    *renderer = new SampleRenderer{renderer_vtable};
    return S_OK;
}

template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
} // namespace

void install_system(Registry &registry) {
    registry.forward("mmdevapi.dll", "mmdevapi.dll");
    registry.add("mmdevapi.dll", "ActivateAudioInterfaceAsync", erase(activate_audio_interface));
    registry.add("mmdevapi.dll", "SetWasapiThreadAffinityMask",
                 erase(set_wasapi_thread_affinity_mask));
    registry.add("mmdevapi.dll", "EnableSpatialAudio", erase(enable_spatial_audio));
    for (const auto *name :
         {"DisableBitstreamOut", "RestoreBitstreamOut", "RefreshWasapiDeviceList"})
        registry.add("mmdevapi.dll", name, erase(nothing));

    registry.forward("mfplat.dll", "mfplat.dll");
    registry.add("mfplat.dll", "MFCreateDXGIDeviceManager", erase(create_device_manager));
    registry.add("mfplat.dll", "MFResetDXGIDeviceManagerX", erase(reset_device_manager));
    registry.add("mfplat.dll", "MFCreateDxvaSampleRendererX", erase(create_sample_renderer));
    for (const auto *name : {"MFCreateDXGISurfaceBufferX", "MFCreateNV12ToRGB32ConverterX",
                             "MFCreateRGB32ToNV12ConverterX"})
        registry.add("mfplat.dll", name, erase(not_implemented));

    for (const auto *name : {"ConfigurePMCs", "ConfigureNBPMCs", "ConfigureL2IPMCs"})
        registry.add("pixevt.dll", name, erase(configure_counters));
    for (const auto *name : {"PIXBeginCapture", "PIXEndCapture"})
        registry.add("pixevt.dll", name, erase(succeeded));
    for (const auto *name :
         {"PIXEventsReplaceBlock", "PIXGetCaptureState", "GetCaptureControlState",
          "RegisterForCaptureControl", "UnregisterForCaptureControl", "SetCaptureCallgraphState",
          "SetCaptureFunctionDetailsState", "SetCaptureFunctionSummaryState",
          "SetCaptureInstructionTraceState"})
        registry.add("pixevt.dll", name, erase(zero));
    registry.add("pixevt.dll", "PIXReportCounter", erase(report_counter));

    registry.forward("toolhelpx.dll", "kernel32.dll");
    registry.add("toolhelpx.dll", "GetSymbolFromAddress", erase(get_symbol_from_address));
    registry.add("toolhelpx.dll", "GetSourceLineFromAddress", erase(not_implemented));
    // a title's crash handler writes a dump of itself: none is written, and what it crashed
    // on is in the log
    registry.add(
        "toolhelpx.dll", "MiniDumpWriteDump",
        erase(+[](HANDLE, DWORD, HANDLE, int, void *, void *, void *) -> BOOL { return FALSE; }));
    registry.add("toolhelpx.dll", "QuerySystemHardwareInfo", erase(query_system_hardware_info));
}
} // namespace xv::runtime::durango
