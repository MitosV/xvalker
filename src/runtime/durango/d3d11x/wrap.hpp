#pragma once
#include <cstddef>
#include <cstdint>
#include <guiddef.h>

// Direct3D 11.X objects in front of the host's Direct3D 11 objects.
//
// A console interface has the methods of the desktop one in a different place: device
// children have two more methods after ID3D11DeviceChild's, resources one more after
// ID3D11Resource's, and the device context is also called through a table of function
// pointers inside the object itself. So the title gets objects of ours with the console
// layout. Each method hands its arguments to the host method, after replacing our objects
// among them by the host's, and wraps the objects the host returns.
namespace xv::runtime::durango::d3d11x {
enum class Kind : std::uint8_t {
    device,
    context,
    swap_chain,
    buffer,
    texture1d,
    texture2d,
    texture3d,
    resource, // one of the four above, whichever the host object is
    view,     // shader resource, render target, depth stencil, unordered access
    state,    // blend, depth stencil, rasterizer, sampler, query: the host has methods past
              // ID3D11DeviceChild
    child,    // shaders, input layouts and anything else with ID3D11DeviceChild's methods only
    dma,      // ID3D11DMAEngineContextX, over a deferred context of the host
    compute,  // ID3D11ComputeContextX, likewise
    // The launch kit's ID3D11DeviceX and ID3D11DeviceContextX: interfaces of their own with
    // the console's methods only, where later kits have them after the desktop's
    device_x,
    context_x,
};
// Our object for a host object, creating it if there is none yet. Takes over the caller's
// reference to `host` and returns one reference to ours.
void *wrap(void *host, Kind kind);
// The host object behind one of ours; `object` itself if it is not ours. No reference taken.
void *unwrap(void *object);
// The kind an object asked for by this interface ID should be wrapped as, if any.
bool kind_of(const GUID &iid, Kind &kind);
// XV_D3D_DEBUG: prints what the host's debug layer has to say since the last call. `where`
// names the method that just ran. Does nothing without the debug layer.
void report_debug_messages(const char *object, const char *where, std::size_t position);
void watch_debug_messages(void *host_device);
} // namespace xv::runtime::durango::d3d11x
