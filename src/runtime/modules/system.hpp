#pragma once
#include <string_view>
#include <windows.h>

namespace xv::runtime {
class Registry;
namespace modules {
// xaudio2_9.dll as a GDK title imports it, by ordinal: the host's engine
void install_xaudio2(Registry &registry);
// XFrontPanelDisplay.dll: no front panel
void install_front_panel(Registry &registry);
// The shader compiler (dxcompiler_x.dll, dxcompiler_xs.dll): the host's dxcompiler.dll if
// there is one next to xvalkyrie or in the system, no compiler otherwise
HRESULT host_dxc_create_instance(REFCLSID class_id, REFIID iid, void **object);
void install_shader_compiler(Registry &registry, std::string_view module);
} // namespace modules
} // namespace xv::runtime
