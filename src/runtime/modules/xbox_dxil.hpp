#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The console's shaders call the console's own intrinsics (@xbox.*), which the host's compiler
// does not know: a shader like that is turned down before it reaches the GPU. This is the
// lowering of those calls to their standard equivalents (a port of americas-recomp's
// `tools/xbox/platform/xbox_dxil_lower.py`, by the same author), so that a console shader can
// be given to the host as it is.
namespace xv::runtime::modules {
// The DXIL of a console shader, lowered.
struct XboxDxilLowering {
    // The container the host takes: the DXIL, lowered and signed again.
    std::vector<std::uint8_t> dxil;
    // The DXIL as text, before and after the lowering.
    std::string text;
    std::string lowered;
    // How many calls were lowered.
    int calls = 0;
    // The console's intrinsics that are still called; empty when the shader is whole.
    std::vector<std::string> left;
    // Whether the signer (dxil.dll) took the container.
    bool signed_ok = false;
};

// The DXIL of a container as text, as `dxc -dumpbin` prints it. Nothing when the container
// holds none, and then `why` says what stopped it.
std::optional<std::string> xbox_dxil_text(std::span<const std::uint8_t> container,
                                          std::string *why = nullptr);

// Whether the container's DXIL calls the console's own intrinsics, which the host's compiler
// does not know.
bool xbox_dxil_has_intrinsics(std::span<const std::uint8_t> container);

// The container's DXIL lowered to what the host understands: every @xbox.* call replaced by
// its standard equivalent, and the container signed again. Nothing when it could not be
// lowered, and then `why` says what stopped it.
std::optional<XboxDxilLowering> lower_xbox_dxil(std::span<const std::uint8_t> container,
                                                std::string *why = nullptr);

// The lowering of the DXIL as text (as `dxc -dumpbin` prints it), for the tools and the tests.
struct XboxDxilText {
    std::string lowered;
    // How many calls were lowered.
    int calls = 0;
    // The console's intrinsics that are still called; empty when the shader is whole.
    std::vector<std::string> left;
};
std::optional<XboxDxilText> lower_xbox_dxil_text(const std::string &text);
} // namespace xv::runtime::modules
