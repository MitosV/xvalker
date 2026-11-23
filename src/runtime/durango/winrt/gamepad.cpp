#include "runtime/durango/winrt/gamepad.hpp"
#include "backends/input/input.hpp"
#include "runtime/durango/winrt/collection.hpp"
#include "runtime/durango/winrt/placeholder.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <inspectable.h>
#include <intrin.h>
#include <windows.h>
#include <winstring.h>

namespace xv::runtime::durango::winrt {
namespace {
constexpr wchar_t class_name[] = L"Windows.Xbox.Input.Gamepad";

// What IGamepad's third method fills in, as titles read it back.
struct RawReading {
    std::uint64_t timestamp;
    std::uint32_t buttons; // xv::input::Button bits
    float left_trigger, right_trigger;
    float left_x, left_y, right_x, right_y;
};
static_assert(sizeof(RawReading) == 40);

// Common part of the two objects here: reference counting, and placeholders for the
// interfaces nobody has looked at yet (the controller interface among them).
template <class Interface> class Object : public Interface {
  public:
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        // IGamepad by its own IID, for titles that ask for it
        constexpr GUID gamepad{
            0x082e402c, 0x07df, 0x45e1, {0xa5, 0xab, 0xa3, 0x12, 0x7a, 0xf1, 0x97, 0xb5}};
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) ||
            iid == __uuidof(IAgileObject) || (iid == gamepad && Interface::is_gamepad)) {
            *object = static_cast<Interface *>(this);
            this->AddRef();
            return S_OK;
        }
        return placeholder_interface(class_name, iid, object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override { return --references_; } // lives with the process
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        *count = 0;
        *iids = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override {
        return WindowsCreateString(class_name, static_cast<UINT32>(wcslen(class_name)), name);
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override {
        *level = BaseTrust;
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
};

// IGamepadReading: the timestamp, the buttons, a flag per button
// and the axes
struct IGamepadReading : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Timestamp(std::uint64_t *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Buttons(std::uint32_t *value) = 0;
    // clang-format off
#define XV_PRESSED(Name) virtual HRESULT STDMETHODCALLTYPE get_Is##Name##Pressed(boolean *) = 0;
    XV_PRESSED(DPadUp)
    XV_PRESSED(DPadDown)
    XV_PRESSED(DPadLeft)
    XV_PRESSED(DPadRight)
    XV_PRESSED(Menu)
    XV_PRESSED(View) XV_PRESSED(LeftThumbstick) XV_PRESSED(RightThumbstick) XV_PRESSED(LeftShoulder)
        XV_PRESSED(RightShoulder) XV_PRESSED(A) XV_PRESSED(B) XV_PRESSED(X) XV_PRESSED(Y)
#undef XV_PRESSED
#define XV_AXIS(Name) virtual HRESULT STDMETHODCALLTYPE get_##Name(float *) = 0;
            XV_AXIS(LeftTrigger) XV_AXIS(RightTrigger) XV_AXIS(LeftThumbstickX)
                XV_AXIS(LeftThumbstickY) XV_AXIS(RightThumbstickX) XV_AXIS(RightThumbstickY)
#undef XV_AXIS
    // clang-format on
};
// GamepadButtons, the same bits as the raw reading
class Reading final : public IGamepadReading {
  public:
    explicit Reading(const RawReading &raw) : raw_(raw) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = this;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        *count = 0;
        *iids = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *name) override {
        constexpr wchar_t reading[] = L"Windows.Xbox.Input.GamepadReading";
        return WindowsCreateString(reading, static_cast<UINT32>(wcslen(reading)), name);
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override {
        *level = BaseTrust;
        return S_OK;
    }
    STDMETHODIMP get_Timestamp(std::uint64_t *value) override { return put(value, raw_.timestamp); }
    STDMETHODIMP get_Buttons(std::uint32_t *value) override { return put(value, raw_.buttons); }
#define XV_PRESSED(Name, Bit)                                                                      \
    STDMETHODIMP get_Is##Name##Pressed(boolean *value) override {                                  \
        return put<boolean>(value, (raw_.buttons & (Bit)) != 0);                                   \
    }
    XV_PRESSED(DPadUp, 0x1);
    XV_PRESSED(DPadDown, 0x2);
    XV_PRESSED(DPadLeft, 0x4);
    XV_PRESSED(DPadRight, 0x8);
    XV_PRESSED(Menu, 0x10);
    XV_PRESSED(View, 0x20);
    XV_PRESSED(LeftThumbstick, 0x40);
    XV_PRESSED(RightThumbstick, 0x80);
    XV_PRESSED(LeftShoulder, 0x100);
    XV_PRESSED(RightShoulder, 0x200);
    XV_PRESSED(A, 0x1000);
    XV_PRESSED(B, 0x2000);
    XV_PRESSED(X, 0x4000);
    XV_PRESSED(Y, 0x8000);
#undef XV_PRESSED
#define XV_AXIS(Name, Field)                                                                       \
    STDMETHODIMP get_##Name(float *value) override { return put(value, raw_.Field); }
    XV_AXIS(LeftTrigger, left_trigger);
    XV_AXIS(RightTrigger, right_trigger);
    XV_AXIS(LeftThumbstickX, left_x);
    XV_AXIS(LeftThumbstickY, left_y);
    XV_AXIS(RightThumbstickX, right_x);
    XV_AXIS(RightThumbstickY, right_y);
#undef XV_AXIS

  private:
    template <class T> static HRESULT put(T *value, T field) {
        if (!value)
            return E_POINTER;
        *value = field;
        return S_OK;
    }
    std::atomic<ULONG> references_{1};
    RawReading raw_;
};

// IGamepad, the default interface: the reading is its third method.
struct IGamepad : IInspectable {
    static constexpr bool is_gamepad = true;
    virtual HRESULT STDMETHODCALLTYPE SetVibration(const float *motors) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(IGamepadReading **reading) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRawCurrentReading(RawReading *reading) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown9(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown10(void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE unknown11(void *, void *) = 0;
};
class Gamepad final : public Object<IGamepad> {
  public:
    STDMETHODIMP GetRawCurrentReading(RawReading *reading) override {
        const auto state = input::poll();
        // once each: that the title reads the gamepad, and that it has seen a button
        static std::atomic<bool> read{false}, pressed{false};
        if (!read.exchange(true))
            std::fprintf(stderr, "[Runtime] the title reads the gamepad\n");
        if (state.buttons && !pressed.exchange(true))
            std::fprintf(stderr, "[Runtime] first gamepad button reached the title (0x%x)\n",
                         state.buttons);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        *reading = {static_cast<std::uint64_t>(now.QuadPart),
                    state.buttons,
                    state.left_trigger,
                    state.right_trigger,
                    state.left_x,
                    state.left_y,
                    state.right_x,
                    state.right_y};
        return S_OK;
    }
    STDMETHODIMP SetVibration(const float *) override { return S_OK; }
    STDMETHODIMP GetCurrentReading(IGamepadReading **reading) override {
        if (!reading)
            return E_POINTER;
        RawReading raw;
        GetRawCurrentReading(&raw);
        *reading = new Reading(raw);
        return S_OK;
    }
#define XV_UNKNOWN(Index)                                                                          \
    STDMETHODIMP unknown##Index(void *first, void *second) override {                              \
        return unknown_method(class_name, "IGamepad", Index, first, second, _ReturnAddress());     \
    }
    XV_UNKNOWN(9) XV_UNKNOWN(10) XV_UNKNOWN(11)
#undef XV_UNKNOWN
};
Gamepad gamepad;

// The statics: the list of gamepads and two events that never happen, since the one
// gamepad is always there.
struct IGamepadStatics : IInspectable {
    static constexpr bool is_gamepad = false;
    virtual HRESULT STDMETHODCALLTYPE get_Gamepads(void **gamepads) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_GamepadAdded(void *handler, std::int64_t *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_GamepadAdded(std::int64_t token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_GamepadRemoved(void *handler, std::int64_t *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_GamepadRemoved(std::int64_t token) = 0;
};
class Statics final : public Object<IGamepadStatics> {
  public:
    // IActivationFactory is asked for by GetActivationFactory itself; the statics are what
    // titles want from this class.
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        constexpr GUID statics{
            0x8c4c1bcd, 0xfdab, 0x4bf8, {0xbd, 0xe9, 0xab, 0xb2, 0xb1, 0xd6, 0xfd, 0xd3}};
        if (object && iid == statics) {
            *object = static_cast<IGamepadStatics *>(this);
            AddRef();
            return S_OK;
        }
        return Object::QueryInterface(iid, object);
    }
    STDMETHODIMP get_Gamepads(void **gamepads) override {
        *gamepads = gamepad_list();
        return S_OK;
    }
    STDMETHODIMP add_GamepadAdded(void *, std::int64_t *token) override {
        *token = ++tokens_;
        return S_OK;
    }
    STDMETHODIMP remove_GamepadAdded(std::int64_t) override { return S_OK; }
    STDMETHODIMP add_GamepadRemoved(void *, std::int64_t *token) override {
        *token = ++tokens_;
        return S_OK;
    }
    STDMETHODIMP remove_GamepadRemoved(std::int64_t) override { return S_OK; }

  private:
    std::atomic<std::int64_t> tokens_{0};
};
Statics statics;
} // namespace

void *gamepad_list() {
    gamepad.AddRef();
    return make_vector_view({static_cast<IGamepad *>(&gamepad)});
}
void *controller() {
    constexpr GUID controller{
        0x9776ff56, 0x9bfd, 0x4581, {0xad, 0x45, 0xb6, 0x45, 0xbb, 0xa5, 0x26, 0xd6}};
    IInspectable *facet = nullptr;
    gamepad.QueryInterface(controller, reinterpret_cast<void **>(&facet));
    return facet;
}
void *controller_list() { return make_vector_view({static_cast<IInspectable *>(controller())}); }
bool gamepad_interface(std::wstring_view name, const GUID &iid, void **object, long &result) {
    constexpr GUID reading{
        0x082e402c, 0x07df, 0x45e1, {0xa5, 0xab, 0xa3, 0x12, 0x7a, 0xf1, 0x97, 0xb5}};
    // Unreal's input skips the controllers that answer for this one
    constexpr GUID other{
        0xc87ec7f7, 0xd0b8, 0x43ac, {0x8a, 0x2f, 0xe1, 0x98, 0x31, 0x79, 0x40, 0x2c}};
    if (name != class_name || (iid != reading && iid != other))
        return false;
    *object = nullptr;
    result = E_NOINTERFACE;
    if (iid == reading) {
        gamepad.AddRef();
        *object = static_cast<IGamepad *>(&gamepad);
        result = S_OK;
    }
    return true;
}
IInspectable *input_class(std::wstring_view name) {
    return name == class_name ? static_cast<IGamepadStatics *>(&statics) : nullptr;
}
} // namespace xv::runtime::durango::winrt
