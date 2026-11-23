#pragma once
#include "runtime/modules/console_shader.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xv::runtime::modules::rdna {
using Stage = ShaderStage;
// One element of a shader's input or output signature. They are kept in the order of their
// registers, and element n is the field `fn` of the HLSL struct made for the signature.
struct Element {
    std::string name;
    std::uint32_t index = 0;
    std::uint32_t type = 3;  // 1 uint, 2 int, 3 float
    std::uint32_t width = 4; // components
    bool system_value = false;
    std::uint32_t reg = 0; // the register it is in, and which of its components
    std::uint8_t mask = 0;
    std::uint8_t read = 0; // the components the shader reads (an input)
};
struct Shader {
    std::span<const std::uint8_t> file;           // the FSSM file: code and what goes with it
    std::span<const std::uint8_t> root_signature; // the RTS0 part
    std::vector<Element> inputs, outputs;
};
// The HLSL for the code: what the shader declares and its `main`. A vertex or pixel shader's
// takes `Input input` and returns `Output`; a compute shader's takes what it needs. Nothing
// if the code has something that is not handled, and then `why` says what and `at` the
// instruction it was met at, in words from the first, or ~0. `needs_sm6` reports whether
// the generated shader uses Shader Model 6 intrinsics.
std::optional<std::string> translate(Stage stage, const Shader &shader, std::string *why = nullptr,
                                     std::uint32_t *at = nullptr, bool *needs_sm6 = nullptr);
} // namespace xv::runtime::modules::rdna
