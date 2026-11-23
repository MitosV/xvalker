#pragma once
#include "runtime/modules/console_shader.hpp"
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <span>
#include <string>
#include <vector>

// XCL's shader ABI for the D3D12.X runtime: console shaders become DXIL through
// xcl_recomp.dll, a pipeline gets XCL's root signature, and a draw or dispatch puts
// the title's root arguments into the memory the translated shader reads.
namespace xv::runtime::modules::d3d12x::xcl {
struct Shader {
    std::vector<std::uint8_t> dxil; // signed DXIL, kept for as long as the process runs
    std::string hlsl;               // the translation, for the static samplers it declares
};
// Whether the recompiler library is there. Nothing of this backend is used without it.
bool available();
// The host device's widest wave: a GPU of 32-lane waves gets wave64 programs as two halves.
void note_device(ID3D12Device *device);
// Translate and compile one console shader. Empty DXIL and `why` say what stopped it.
const Shader *shader(modules::ShaderStage stage, std::span<const std::uint8_t> container,
                     std::string *why);
// XCL's six-root-UAV signature with the static samplers named by the pipeline's HLSL.
ID3D12RootSignature *abi_root_signature(ID3D12Device *device, bool graphics,
                                        const std::vector<const Shader *> &shaders);
// Pipelines made with XCL's ABI, and the signature each one needs at draw time.
void remember_pipeline(ID3D12PipelineState *pipeline, ID3D12RootSignature *signature,
                       bool graphics);
bool translated_pipeline(ID3D12PipelineState *pipeline, bool &graphics);
// The layout of a title's root signature, to pack its arguments as XCL does.
void remember_root_signature(const void *signature, const void *blob, std::size_t size);
void set_root_signature(void *list, const void *signature, bool graphics);
void set_pipeline(void *list, ID3D12PipelineState *pipeline);
void clear_list(void *list);
// Capture what the title writes into the console's root arguments.
void write_constant(void *list, UINT parameter, UINT offset, UINT count, const void *data,
                    bool graphics);
void write_view(void *list, UINT parameter, std::uint64_t address, bool graphics);
void write_table(void *list, UINT parameter, std::uint64_t address, bool graphics);
// Before a draw with a translated pipeline: bind the ABI and its six UAVs.
enum class Binding {
    bound,        // the ABI is bound: the draw goes
    untranslated, // the pipeline is not a translated one: the draw goes as the title's own
    skipped       // the ABI could not be bound: the draw must not be recorded
};
Binding before_draw(void *list, ID3D12GraphicsCommandList *host);
} // namespace xv::runtime::modules::d3d12x::xcl
