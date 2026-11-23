#include "runtime/modules/system.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <windows.h>

#include <objbase.h> // after windows.h

namespace xv::runtime::modules {
namespace {
HMODULE host_xaudio2() {
    static const auto library =
        LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return library;
}
// The processor argument names console cores, so the host's default is used instead
HRESULT __cdecl xaudio2_create(void **engine, UINT32 flags, UINT32) {
    using Create = HRESULT(__cdecl *)(void **, UINT32, UINT32);
    const auto create =
        host_xaudio2() ? reinterpret_cast<Create>(GetProcAddress(host_xaudio2(), "XAudio2Create"))
                       : nullptr;
    // the host's engine makes its voices through COM, which a console title never starts
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto result = create ? create(engine, flags & 0x8000, 1) : E_NOTIMPL;
    std::fprintf(stderr, "[Runtime] XAudio2 engine on the host: 0x%08lx\n",
                 static_cast<unsigned long>(result));
    std::fflush(stderr);
    return result;
}
HRESULT WINAPI dxc_create_instance(const GUID *class_id, const GUID *iid, void **object) {
    if (!class_id || !iid || !object)
        return E_POINTER;
    const auto result = host_dxc_create_instance(*class_id, *iid, object);
    static std::atomic<bool> shown{false};
    if (result == CLASS_E_CLASSNOTAVAILABLE && !shown.exchange(true))
        std::fprintf(stderr, "[Runtime] the title asks for the shader compiler, and there "
                             "is no dxcompiler.dll next to xvalkyrie or in the system\n");
    return result;
}
} // namespace

HRESULT host_dxc_create_instance(REFCLSID class_id, REFIID iid, void **object) {
    using Create = HRESULT(WINAPI *)(REFCLSID, REFIID, void **);
    static const auto create = [] {
        auto library =
            LoadLibraryExW((host::executable_directory() / "dxcompiler.dll").c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            library = LoadLibraryExW(L"dxcompiler.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return library ? reinterpret_cast<Create>(GetProcAddress(library, "DxcCreateInstance"))
                       : nullptr;
    }();
    *object = nullptr;
    return create ? create(class_id, iid, object) : CLASS_E_CLASSNOTAVAILABLE;
}

void install_xaudio2(Registry &registry) {
    const char *const module = "xaudio2_9.dll";
    registry.add(module, "XAudio2Create", reinterpret_cast<void *>(&xaudio2_create));
    registry.add(module, "#1", reinterpret_cast<void *>(&xaudio2_create));
    // the console's ordinals for what the host exports by name
    const std::pair<const char *, const char *> named[] = {
        {"#2", "CreateAudioReverb"},  {"#3", "CreateAudioVolumeMeter"},
        {"#4", "CreateFX"},           {"#5", "X3DAudioCalculate"},
        {"#6", "X3DAudioInitialize"}, {"#7", "CreateAudioReverbV2_8"}};
    for (const auto &[ordinal, name] : named)
        if (auto *const function =
                host_xaudio2() ? GetProcAddress(host_xaudio2(), name) : nullptr) {
            registry.add(module, ordinal, reinterpret_cast<void *>(function));
            registry.add(module, name, reinterpret_cast<void *>(function));
        }
}
// The front panel is a development kit's: a retail console has none, and says so. Its size
// and pixel format are answered all the same, with a kit's 256x64 of 8-bit grey, because
// titles ask for them before asking whether there is a panel. What a retail console says
// there is not known.
bool WINAPI front_panel_supported() { return false; }
HRESULT WINAPI front_panel_absent() { return E_NOTIMPL; }
HRESULT WINAPI front_panel_dimensions(std::uint32_t *height, std::uint32_t *width) {
    if (!height || !width)
        return E_POINTER;
    *height = 64;
    *width = 256;
    return S_OK;
}
HRESULT WINAPI front_panel_format(std::uint32_t *format) {
    if (!format)
        return E_POINTER;
    *format = 61; // DXGI_FORMAT_R8_UNORM
    return S_OK;
}
HRESULT WINAPI front_panel_present(std::uint32_t, const std::uint8_t *) { return S_OK; }
void install_front_panel(Registry &registry) {
    const char *const module = "xfrontpaneldisplay.dll";
    const auto add = [&](const char *name, auto *function) {
        registry.add(module, name, reinterpret_cast<void *>(function));
    };
    add("XFrontPanelIsSupported", &front_panel_supported);
    add("XFrontPanelGetScreenDimensions", &front_panel_dimensions);
    add("XFrontPanelGetScreenPixelFormat", &front_panel_format);
    add("XFrontPanelPresentBuffer", &front_panel_present);
    for (const auto *name : {"XFrontPanelGetButtonStates", "XFrontPanelGetLightStates",
                             "XFrontPanelSetLightStates", "XFrontPanelSetDisplayName"})
        add(name, &front_panel_absent);
}
void install_shader_compiler(Registry &registry, std::string_view module) {
    registry.add(module, "DxcCreateInstance", reinterpret_cast<void *>(&dxc_create_instance));
}
} // namespace xv::runtime::modules
