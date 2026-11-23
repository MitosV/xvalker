#include "backends/input/input.hpp"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string_view>

namespace xv::input {
namespace detail {
// state of the system's keyboard and first gamepad, read directly
bool direct_key(int key);
bool direct_gamepad(GamepadState &state);
} // namespace detail
namespace {
struct Description {
    const char *id, *name;
    int default_key;
};
// in the order of Control. Defaults: sticks on WASD and the arrows, face buttons around
// the right hand.
constexpr Description controls[control_count] = {
    {"a", "A", 0x20 /* space */},
    {"b", "B", 0x08 /* backspace */},
    {"x", "X", 'Q'},
    {"y", "Y", 'E'},
    {"menu", "Menu", 0x0d /* enter */},
    {"view", "View", 0x09 /* tab */},
    {"dpad_up", "D-pad up", 'T'},
    {"dpad_down", "D-pad down", 'G'},
    {"dpad_left", "D-pad left", 'F'},
    {"dpad_right", "D-pad right", 'H'},
    {"left_shoulder", "Left bumper", '1'},
    {"right_shoulder", "Right bumper", '2'},
    {"left_trigger", "Left trigger", 'Z'},
    {"right_trigger", "Right trigger", 'C'},
    {"left_thumbstick", "Left stick click", 'R'},
    {"right_thumbstick", "Right stick click", 'V'},
    {"left_stick_up", "Left stick up", 'W'},
    {"left_stick_down", "Left stick down", 'S'},
    {"left_stick_left", "Left stick left", 'A'},
    {"left_stick_right", "Left stick right", 'D'},
    {"right_stick_up", "Right stick up", 0x26},
    {"right_stick_down", "Right stick down", 0x28},
    {"right_stick_left", "Right stick left", 0x25},
    {"right_stick_right", "Right stick right", 0x27},
};
constexpr std::uint32_t button_of[] = {a,
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
                                       0,
                                       0,
                                       left_thumbstick,
                                       right_thumbstick};

std::mutex mutex;
Settings current = default_settings();
std::bitset<256> window_keys;
} // namespace

const char *control_name(Control control) {
    return controls[static_cast<std::size_t>(control)].name;
}
const char *control_id(Control control) { return controls[static_cast<std::size_t>(control)].id; }

Settings default_settings() {
    Settings result;
    for (std::size_t index = 0; index < control_count; ++index)
        result.keys[index] = controls[index].default_key;
    return result;
}

std::string key_name(int key) {
    if (key <= 0 || key >= 256)
        return "(none)";
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
        return std::string(1, static_cast<char>(key));
    if (key >= 0x70 && key <= 0x7b)
        return "F" + std::to_string(key - 0x6f);
    if (key >= 0x60 && key <= 0x69)
        return "Numpad " + std::to_string(key - 0x60);
    switch (key) {
    case 0x08:
        return "Backspace";
    case 0x09:
        return "Tab";
    case 0x0d:
        return "Enter";
    case 0x10:
        return "Shift";
    case 0x11:
        return "Ctrl";
    case 0x12:
        return "Alt";
    case 0x1b:
        return "Escape";
    case 0x20:
        return "Space";
    case 0x21:
        return "Page Up";
    case 0x22:
        return "Page Down";
    case 0x23:
        return "End";
    case 0x24:
        return "Home";
    case 0x25:
        return "Left";
    case 0x26:
        return "Up";
    case 0x27:
        return "Right";
    case 0x28:
        return "Down";
    case 0x2d:
        return "Insert";
    case 0x2e:
        return "Delete";
    case 0xba:
        return ";";
    case 0xbb:
        return "=";
    case 0xbc:
        return ",";
    case 0xbd:
        return "-";
    case 0xbe:
        return ".";
    case 0xbf:
        return "/";
    case 0xc0:
        return "`";
    case 0xdb:
        return "[";
    case 0xdc:
        return "\\";
    case 0xdd:
        return "]";
    case 0xde:
        return "'";
    default:
        return "Key " + std::to_string(key);
    }
}

GamepadState from_keys(const Settings &settings, const std::function<bool(int key)> &pressed) {
    const auto down = [&](Control control) {
        const auto key = settings.keys[static_cast<std::size_t>(control)];
        return key > 0 && key < 256 && pressed(key);
    };
    GamepadState state;
    for (std::size_t index = 0; index < std::size(button_of); ++index)
        if (button_of[index] && down(static_cast<Control>(index)))
            state.buttons |= button_of[index];
    state.left_trigger = down(Control::left_trigger) ? 1.f : 0.f;
    state.right_trigger = down(Control::right_trigger) ? 1.f : 0.f;
    const auto axis = [&](Control positive, Control negative) {
        return static_cast<float>(int{down(positive)} - int{down(negative)});
    };
    state.left_x = axis(Control::left_stick_right, Control::left_stick_left);
    state.left_y = axis(Control::left_stick_up, Control::left_stick_down);
    state.right_x = axis(Control::right_stick_right, Control::right_stick_left);
    state.right_y = axis(Control::right_stick_up, Control::right_stick_down);
    return state;
}

GamepadState merge(const GamepadState &first, const GamepadState &second) {
    const auto further = [](float one, float other) {
        return std::abs(one) >= std::abs(other) ? one : other;
    };
    return {first.buttons | second.buttons,
            std::max(first.left_trigger, second.left_trigger),
            std::max(first.right_trigger, second.right_trigger),
            further(first.left_x, second.left_x),
            further(first.left_y, second.left_y),
            further(first.right_x, second.right_x),
            further(first.right_y, second.right_y)};
}

void configure(const Settings &settings) {
    const std::lock_guard lock(mutex);
    current = settings;
}
Settings settings() {
    const std::lock_guard lock(mutex);
    return current;
}
void window_key(int key, bool pressed) {
    if (key <= 0 || key >= 256)
        return;
    const std::lock_guard lock(mutex);
    window_keys[static_cast<std::size_t>(key)] = pressed;
}
void window_focus_lost() {
    const std::lock_guard lock(mutex);
    window_keys.reset();
}

namespace {
GamepadState read();
// XV_INPUT_PRESS=<control id>@<seconds>[,<control id>@<seconds>...]: holds each control
// for half a second, that long after the first reading. For checking that input reaches a
// title, and for walking through its menus, without a person.
GamepadState scripted() {
    using Clock = std::chrono::steady_clock;
    static const char *const script = std::getenv("XV_INPUT_PRESS");
    GamepadState state;
    if (!script)
        return state;
    static const auto start = Clock::now();
    const double now = std::chrono::duration<double>(Clock::now() - start).count();
    Settings held;
    std::string_view rest = script;
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const auto step = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        const auto at = step.find('@');
        if (at == std::string_view::npos)
            continue;
        const double when = std::atof(std::string(step.substr(at + 1)).c_str());
        if (now < when || now > when + .5)
            continue;
        for (std::size_t index = 0; index < control_count; ++index)
            if (step.substr(0, at) == controls[index].id)
                held.keys[index] = 1;
    }
    return from_keys(held, [](int key) { return key == 1; });
}
std::array<std::chrono::steady_clock::time_point, control_count> held_until{};
GamepadState pressed() {
    Settings held;
    {
        const std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < control_count; ++index)
            held.keys[index] = now < held_until[index];
    }
    return from_keys(held, [](int key) { return key == 1; });
}
} // namespace

bool press(std::string_view id, double seconds) {
    for (std::size_t index = 0; index < control_count; ++index)
        if (id == controls[index].id) {
            const std::lock_guard lock(mutex);
            held_until[index] = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(seconds));
            return true;
        }
    return false;
}
GamepadState poll() { return merge(merge(scripted(), pressed()), read()); }

namespace {
GamepadState read() {
    Settings bindings;
    std::bitset<256> keys;
    {
        const std::lock_guard lock(mutex);
        bindings = current;
        keys = window_keys;
    }
    auto state =
        bindings.capture == Capture::direct
            ? from_keys(bindings, detail::direct_key)
            : from_keys(bindings, [&](int key) { return keys[static_cast<std::size_t>(key)]; });
    // The window sees no gamepad events, so the host gamepad is read the same way under
    // both captures (and only while a window of this process is in the foreground).
    GamepadState gamepad;
    if (bindings.host_gamepad && detail::direct_gamepad(gamepad))
        state = merge(state, gamepad);
    return state;
}
} // namespace
} // namespace xv::input
