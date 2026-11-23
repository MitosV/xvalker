#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// What the emulated gamepad reads, wherever it comes from. Console runtimes ask poll();
// the window and the system feed it, depending on the capture method chosen.
namespace xv::input {
// Button bits as console titles get them (Windows.Xbox.Input.GamepadButtons).
enum Button : std::uint32_t {
    menu = 0x4,
    view = 0x8,
    a = 0x10,
    b = 0x20,
    x = 0x40,
    y = 0x80,
    dpad_up = 0x100,
    dpad_down = 0x200,
    dpad_left = 0x400,
    dpad_right = 0x800,
    left_shoulder = 0x1000,
    right_shoulder = 0x2000,
    left_thumbstick = 0x4000,
    right_thumbstick = 0x8000,
};
struct GamepadState {
    std::uint32_t buttons = 0;
    float left_trigger = 0, right_trigger = 0;              // 0 to 1
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0; // -1 to 1, up and right positive
};
// Everything a key can be bound to: the buttons, the triggers, and each direction of
// each stick.
enum class Control {
    a,
    b,
    x,
    y,
    menu,
    view,
    dpad_up,
    dpad_down,
    dpad_left,
    dpad_right,
    left_shoulder,
    right_shoulder,
    left_trigger,
    right_trigger,
    left_thumbstick,
    right_thumbstick,
    left_stick_up,
    left_stick_down,
    left_stick_left,
    left_stick_right,
    right_stick_up,
    right_stick_down,
    right_stick_left,
    right_stick_right,
    count
};
inline constexpr std::size_t control_count = static_cast<std::size_t>(Control::count);
const char *control_name(Control control); // for people
const char *control_id(Control control);   // for the settings file

// How key and gamepad state is obtained.
enum class Capture {
    window, // keyboard from the events of the title's window, while it has the focus
    direct  // keyboard from the system, without going through the window
    // either way the host gamepad (XInput) is read too, unless host_gamepad is off
};
// Keys are Windows virtual-key codes; 0 is "not bound".
struct Settings {
    Capture capture = Capture::window;
    std::array<int, control_count> keys{};
    bool host_gamepad = true; // also read the first gamepad of the host
};
Settings default_settings();
std::string key_name(int key);

// The gamepad state a set of pressed keys amounts to under these bindings.
GamepadState from_keys(const Settings &settings, const std::function<bool(int key)> &pressed);
// The state with the most of each control: for a keyboard and a gamepad used together.
GamepadState merge(const GamepadState &first, const GamepadState &second);

// Process-wide state, safe from any thread.
void configure(const Settings &settings);
Settings settings();
// window capture: what the window saw
void window_key(int key, bool pressed);
void window_focus_lost();
GamepadState poll();
// Holds a control, named as in the settings file, for that long. False for an unknown name.
bool press(std::string_view id, double seconds);
} // namespace xv::input
