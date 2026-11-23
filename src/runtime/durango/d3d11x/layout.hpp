#pragma once
#include "runtime/durango/d3d11x/wrap.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace xv::runtime::durango::d3d11x {
// What to do with one argument of a method. Argument 0 is the object itself.
struct Fix {
    enum Type : std::uint8_t {
        none,
        in,          // one of our objects, may be null
        in_array,    // array of our objects; its length is argument `other`
        out,         // T **result of a method that returns nothing
        out_checked, // T **result of a method that returns an HRESULT
        out_iid,     // void **result whose interface ID is argument `other`
        out_array,   // array of results of a method that returns nothing; length in `other`
    } type = none;
    std::uint8_t argument = 0;
    std::uint8_t other = 0;
    Kind kind = Kind::child;
};
struct Method {
    enum Action : std::uint8_t {
        unknown, // nobody knows what is here: report the call and fail
        forward, // call slot `host` of the host object
        ignore,  // console-only and without effect here
        custom,  // `function` goes into the slot as it is
    } action = unknown;
    std::uint16_t host = 0;
    std::array<Fix, 3> fixes{};
    void *function = nullptr;
    bool draws = false; // needs the state the title left in the object applied first
};
inline constexpr std::size_t vtable_slots = 256, object_slots = 512;
struct Layout {
    const char *name;
    // size of the object in pointers; more than 1 only if the title reaches into it
    std::size_t words = 1;
    std::array<Method, vtable_slots> vtable{};
    // methods called through pointers stored in the object, by offset / 8
    std::array<Method, object_slots> object{};
    // where that table starts in the object: the word of its QueryInterface
    std::size_t table = 124;
    // the byte of a context that makes titles call SetFastResources_Debug with a table of
    // what to bind, where they would write the GPU's packets themselves
    std::size_t out_of_line = 0x390;
    // applies state the title wrote into the object instead of calling a method
    void (*apply_state)(void *object) = nullptr;
};
const Layout &layout(Kind kind);
// Whether the title was built with the first development kits, whose DXGI interfaces have
// one more method than later ones: swap chains made from then on have their layout.
// `launch` is the kit before those, with the desktop's interfaces; `earliest` is 1 or 2 for
// the two kits after it (the later one first), whose context still has the desktop's whole
// table in the object.
void first_development_kits(bool yes, bool launch = false, int earliest = 0);
bool first_development_kits();
bool launch_kit();
// IDXGISwapChain::Present of a swap chain of ours
long present_swap_chain(void *swap_chain, unsigned interval); // HRESULT
// sets the events a title gave DXGIXSetFrameNotification; after each frame presented
void signal_frame_notifications();
} // namespace xv::runtime::durango::d3d11x
