#include "runtime/modules/d3d12x/xcl_backend.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/console_shader.hpp"
#include "runtime/modules/d3d12x/console_heap.hpp"
#include "runtime/modules/d3d12x/uma.hpp"
#include "runtime/modules/system.hpp"
#include "runtime/modules/xcl_recomp.h"

#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <windows.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xv::runtime::modules::d3d12x::xcl {
namespace {
using Bytes = std::span<const std::uint8_t>;
constexpr int shader_version = 3;
constexpr std::size_t kernel_sgprs = 128;
constexpr std::size_t console_grain = 0x10000;

struct Library {
    HMODULE module = nullptr;
    int32_t(STDMETHODCALLTYPE *translate)(const std::uint8_t *, std::size_t, std::uint32_t,
                                         XclResult *) = nullptr;
    void(STDMETHODCALLTYPE *free_result)(XclResult *) = nullptr;
};
Library library() {
    static Library found = [] {
        Library result;
        const auto path = host::executable_directory() / L"xcl_recomp.dll";
        result.module = LoadLibraryExW(path.c_str(), nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                           LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!result.module)
            result.module = LoadLibraryExW(L"xcl_recomp.dll", nullptr,
                                           LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (result.module) {
            result.translate = reinterpret_cast<decltype(result.translate)>(
                GetProcAddress(result.module, "xcl_translate"));
            result.free_result = reinterpret_cast<decltype(result.free_result)>(
                GetProcAddress(result.module, "xcl_free"));
        }
        if (!result.translate || !result.free_result) {
            if (static std::atomic<bool> shown{false}; !shown.exchange(true))
                std::fprintf(stderr, "[Runtime] xcl_recomp.dll has no xcl_translate/xcl_free\n");
            result = {};
        } else {
            std::fprintf(stderr, "[Runtime] XCL shader recompiler loaded\n");
        }
        return result;
    }();
    return found;
}

const wchar_t *profile_of(modules::ShaderStage stage) {
    switch (stage) {
    case modules::ShaderStage::vertex: return L"vs_6_6";
    case modules::ShaderStage::pixel: return L"ps_6_6";
    case modules::ShaderStage::compute: return L"cs_6_6";
    default: return nullptr;
    }
}
const char *profile_name_of(modules::ShaderStage stage) {
    switch (stage) {
    case modules::ShaderStage::vertex: return "vs_6_6";
    case modules::ShaderStage::pixel: return "ps_6_6";
    case modules::ShaderStage::compute: return "cs_6_6";
    default: return "";
    }
}
std::string name_of(Bytes bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto byte : bytes)
        hash = (hash ^ byte) * 0x100000001b3ull;
    char text[32];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}
std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool write_file(const std::filesystem::path &path, Bytes bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto partial = path;
    partial += "." + std::to_string(GetCurrentProcessId());
    {
        std::ofstream file(partial, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    std::filesystem::rename(partial, path, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return false;
    }
    return true;
}
std::vector<std::uint8_t> compile(modules::ShaderStage stage, const std::string &hlsl,
                                  std::string *why) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(host_dxc_create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        return {};
    const wchar_t *arguments[] = {L"-E", L"main", L"-T", profile_of(stage), L"-HV", L"2021"};
    const DxcBuffer input{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&input, arguments, static_cast<UINT>(std::size(arguments)), nullptr,
                                 IID_PPV_ARGS(&result))))
        return {};
    HRESULT status = E_FAIL;
    ComPtr<IDxcBlob> code;
    if (FAILED(result->GetStatus(&status)) || FAILED(status) ||
        FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr)) || !code) {
        if (why) {
            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors)
                why->assign(errors->GetStringPointer(), errors->GetStringLength());
            else
                *why = "DXC turned the XCL shader down";
        }
        return {};
    }
    const auto *const data = static_cast<const std::uint8_t *>(code->GetBufferPointer());
    return {data, data + code->GetBufferSize()};
}

std::mutex shaders_mutex;
std::map<std::string, Shader> shaders; // by stage and the hash of the console container

// XV_XCL_DUMP_HLSL=DIR: the translation of every shader, for the diagnostics of a bad draw.
void dump_hlsl(modules::ShaderStage stage, const std::string &key, const std::string &hlsl) {
    static const auto folder = [] {
        const char *where = std::getenv("XV_XCL_DUMP_HLSL");
        return where ? std::filesystem::path{where} : std::filesystem::path{};
    }();
    if (folder.empty())
        return;
    const char *stage_name[] = {"v", "p", "c"};
    const auto path = folder / (std::string{stage_name[static_cast<int>(stage)]} + "-" + key +
                               ".hlsl");
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    std::ofstream file(path);
    if (file)
        file << hlsl;
}

// XV_XCL_TRACE: what the shaders of a draw read: the root arguments in the user SGPRs and
// the descriptors they name.
bool trace_draws() {
    static const bool on = std::getenv("XV_XCL_TRACE") != nullptr;
    return on;
}
void trace_arguments(const std::vector<std::uint8_t> &args, std::size_t bytes) {
    const auto words = bytes / 4;
    std::fprintf(stderr, "[Runtime] XCL draw: %zu root argument words:", words);
    for (std::size_t k = 0; k < words && k < 32; ++k) {
        std::uint32_t word = 0;
        std::memcpy(&word, args.data() + k * 4, 4);
        std::fprintf(stderr, " %08x", word);
    }
    std::fprintf(stderr, "\n");
    for (std::size_t k = 0; k < words && k < 32; ++k) {
        std::uint32_t word = 0;
        std::memcpy(&word, args.data() + k * 4, 4);
        std::uint32_t descriptor[8]{};
        if (!console_heap::words_at(word, descriptor, 8))
            continue;
        std::fprintf(stderr, "[Runtime]   argument %zu at %08x: %08x %08x %08x %08x %08x %08x "
                             "%08x %08x\n",
                     k, word, descriptor[0], descriptor[1], descriptor[2], descriptor[3],
                     descriptor[4], descriptor[5], descriptor[6], descriptor[7]);
    }
    std::fflush(stderr);
}

ID3D12RootSignature *make_abi(ID3D12Device *device, bool graphics, UINT sampler_count) {
    D3D12_ROOT_PARAMETER1 parameters[6]{};
    for (UINT k = 0; k < 6; ++k) {
        parameters[k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[k].Descriptor.ShaderRegister = k;
        parameters[k].Descriptor.RegisterSpace = 0;
        parameters[k].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameters[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(sampler_count);
    for (UINT k = 0; k < sampler_count; ++k) {
        auto &sampler = samplers[k];
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.ShaderRegister = k;
        sampler.RegisterSpace = 1;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC1 desc{};
    desc.NumParameters = 6;
    desc.pParameters = parameters;
    desc.NumStaticSamplers = sampler_count;
    desc.pStaticSamplers = samplers.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                 D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED |
                 (graphics ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                           : D3D12_ROOT_SIGNATURE_FLAG_NONE);
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1 = desc;
    using Serialize = HRESULT(WINAPI *)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **,
                                        ID3DBlob **);
    static const auto serialize = [] {
        const auto module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<Serialize>(
                            GetProcAddress(module, "D3D12SerializeVersionedRootSignature"))
                      : nullptr;
    }();
    if (!serialize)
        return nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if (FAILED(serialize(&versioned, blob.GetAddressOf(), nullptr)) || !blob)
        return nullptr;
    ID3D12RootSignature *signature = nullptr;
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&signature))))
        return nullptr;
    return signature;
}
UINT sampler_count_of(const std::vector<const Shader *> &shaders) {
    UINT count = 0;
    for (const auto *shader : shaders)
        if (shader)
            for (std::size_t at = 0; (at = shader->hlsl.find("xcl_ss", at)) != std::string::npos;
                 ++at) {
                const auto reg = shader->hlsl.find("register(s", at);
                if (reg == std::string::npos || reg > at + 32)
                    break;
                const auto number = std::strtoul(shader->hlsl.c_str() + reg + 9, nullptr, 10);
                count = std::max(count, static_cast<UINT>(number) + 1);
            }
    return count;
}
std::mutex signatures_mutex;
std::map<std::pair<bool, UINT>, ID3D12RootSignature *> abis;

struct Param {
    std::size_t at = 0, bytes = 0;
};
struct Layout {
    std::vector<Param> params;
    std::size_t bytes = 0, compute_inline = 0, graphics_inline = 0;
};
std::mutex layouts_mutex;
std::map<const void *, Layout> layouts;

struct Pipeline {
    ID3D12RootSignature *signature = nullptr;
    bool graphics = false;
};
std::mutex pipelines_mutex;
std::map<ID3D12PipelineState *, Pipeline> pipelines;

struct ListState {
    const void *root = nullptr;
    bool graphics = false;
    ID3D12PipelineState *pipeline = nullptr;
    std::vector<std::uint8_t> args;
};
std::mutex lists_mutex;
std::map<void *, ListState> lists;
ListState &state_of(void *list) {
    const std::lock_guard lock(lists_mutex);
    return lists[list];
}

// The bytes a list's draws read stay put until the list is reset: the title can reset a list
// only once the GPU is done with the one it recorded before. The blocks are in window 0, the
// one the console's 32-bit addresses name, and are given back when the list is reset.
// XCL's ARENA_BLOCK: 4 × 64 KiB grain
constexpr std::size_t arena_block = 4 * console_grain;
struct Block {
    std::uint8_t *cpu = nullptr;
    std::uintptr_t console = 0;
    std::size_t size = 0;
    std::size_t next = 0;
};
struct Arena {
    std::vector<Block> blocks;
};
std::mutex arenas_mutex;
std::map<void *, Arena> arenas;
std::uint64_t place_in_ring(void *list, const void *data, std::size_t bytes) {
    const std::lock_guard lock(arenas_mutex);
    Arena &arena = arenas[list];
    const auto aligned = (bytes + 255) & ~std::size_t{255};
    if (arena.blocks.empty() || arena.blocks.back().next + aligned > arena.blocks.back().size) {
        Block block;
        block.cpu = static_cast<std::uint8_t *>(uma_alloc_in(0, arena_block, 0x10000));
        if (!block.cpu)
            return 0;
        block.console = reinterpret_cast<std::uintptr_t>(block.cpu);
        block.size = arena_block;
        arena.blocks.push_back(block);
    }
    auto &block = arena.blocks.back();
    std::memcpy(block.cpu + block.next, data, bytes);
    const auto console = block.console + block.next;
    block.next += aligned;
    return console;
}
// The list is reset: the memory its draws read is free again.
void free_ring(void *list) {
    const std::lock_guard lock(arenas_mutex);
    const auto found = arenas.find(list);
    if (found == arenas.end())
        return;
    for (const auto &block : found->second.blocks)
        uma_free(block.cpu);
    arenas.erase(found);
}
// the widest wave of the host's GPU, from its device (64 until one is known)
std::atomic<UINT> host_wave_lanes{64};
} // namespace

bool available() { return library().module != nullptr; }

void note_device(ID3D12Device *device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
    if (!device || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options,
                                                      sizeof options)))
        return;
    host_wave_lanes = options.WaveLaneCountMax;
    std::fprintf(stderr, "[Runtime] XCL: the host's waves are %u to %u lanes\n",
                 options.WaveLaneCountMin, options.WaveLaneCountMax);
}

const Shader *shader(modules::ShaderStage stage, Bytes container, std::string *why) {
    if (!available() || !profile_of(stage))
        return nullptr;
    const auto key = std::string(1, "vpc"[static_cast<int>(stage)]) + "-" + name_of(container) +
                     "-xcl-v" + std::to_string(shader_version) + "-w" +
                     std::to_string(host_wave_lanes.load());
    {
        const std::lock_guard lock(shaders_mutex);
        if (const auto found = shaders.find(key); found != shaders.end())
            return &found->second;
    }
    XclResult result{};
    const auto status = library().translate(container.data(), container.size(),
                                            host_wave_lanes < 64 ? XCL_FLAG_HOST_WAVE32 : XCL_FLAG_NONE,
                                            &result);
    if (status != 0) {
        if (why && result.error)
            *why = result.error;
        library().free_result(&result);
        return nullptr;
    }
    const auto wanted = profile_name_of(stage);
    const bool profile = result.profiles && std::strstr(result.profiles, wanted);
    Shader made;
    if (profile && result.hlsl) {
        made.hlsl = result.hlsl;
        dump_hlsl(stage, key, made.hlsl);
        const auto &folder = shader_cache_directory();
        const auto name = key + ".dxil";
        made.dxil = folder.empty() ? std::vector<std::uint8_t>{} : read_file(folder / name);
        if (made.dxil.empty()) {
            made.dxil = compile(stage, made.hlsl, why);
            if (!made.dxil.empty() && !folder.empty())
                write_file(folder / name, made.dxil);
        }
    } else if (why) {
        *why = "the recompiler made no shader for this stage";
    }
    library().free_result(&result);
    if (made.dxil.empty())
        return nullptr;
    const std::lock_guard lock(shaders_mutex);
    return &shaders.emplace(key, std::move(made)).first->second;
}

ID3D12RootSignature *abi_root_signature(ID3D12Device *device, bool graphics,
                                        const std::vector<const Shader *> &shaders) {
    const auto count = sampler_count_of(shaders);
    const std::lock_guard lock(signatures_mutex);
    const auto key = std::make_pair(graphics, count);
    if (const auto found = abis.find(key); found != abis.end())
        return found->second;
    auto *const signature = make_abi(device, graphics, count);
    if (signature)
        abis.emplace(key, signature);
    return signature;
}

void remember_pipeline(ID3D12PipelineState *pipeline, ID3D12RootSignature *signature,
                       bool graphics) {
    if (!pipeline || !signature)
        return;
    const std::lock_guard lock(pipelines_mutex);
    pipelines[pipeline] = {signature, graphics};
}
bool translated_pipeline(ID3D12PipelineState *pipeline, bool &graphics) {
    const std::lock_guard lock(pipelines_mutex);
    const auto found = pipelines.find(pipeline);
    if (found == pipelines.end())
        return false;
    graphics = found->second.graphics;
    return true;
}

std::uint32_t rts_word(Bytes blob, std::size_t at) {
    std::uint32_t value = 0;
    if (at + 4 <= blob.size())
        std::memcpy(&value, blob.data() + at, 4);
    return value;
}
// The title's root signature is a DXBC container with an RTS0 part, not a serialized
// D3D12 root signature: read the console's layout the way XCL's console_list.rs does.
Bytes rts0_part(Bytes blob) {
    if (blob.size() < 32 || std::memcmp(blob.data(), "DXBC", 4))
        return blob;
    const auto count = rts_word(blob, 28);
    for (UINT i = 0; i < count && 36 + 4 * i <= blob.size(); ++i) {
        const auto at = rts_word(blob, 32 + 4 * i);
        if (at + 8 > blob.size() || std::memcmp(blob.data() + at, "RTS0", 4))
            continue;
        const auto size = rts_word(blob, at + 4);
        if (size <= blob.size() - at - 8)
            return blob.subspan(at + 8, size);
    }
    return {};
}
void remember_root_signature(const void *signature, const void *blob, std::size_t size) {
    if (!signature)
        return;
    const auto rts0 = rts0_part({static_cast<const std::uint8_t *>(blob), size});
    const auto count = rts_word(rts0, 4), parameters = rts_word(rts0, 8);
    if (!count || count > 64 || parameters + 12ull * count > rts0.size())
        return;
    Layout layout;
    bool compute_spilled = false, graphics_spilled = false;
    for (UINT i = 0; i < count; ++i) {
        const auto at = parameters + 12ull * i;
        const auto type = rts_word(rts0, at), pay = rts_word(rts0, at + 8);
        const std::size_t bytes =
            type == 1 ? 4ull * rts_word(rts0, pay + 8) : type >= 2 && type <= 4 ? 8 : 4;
        layout.params.push_back({layout.bytes, bytes});
        compute_spilled |= layout.compute_inline + bytes > 48;
        if (!compute_spilled)
            layout.compute_inline += bytes;
        graphics_spilled |= layout.graphics_inline + bytes > 96;
        if (!graphics_spilled)
            layout.graphics_inline += bytes;
        layout.bytes += bytes;
    }
    const std::lock_guard lock(layouts_mutex);
    layouts[signature] = std::move(layout);
}
void set_root_signature(void *list, const void *signature, bool graphics) {
    auto &entry = state_of(list);
    entry.root = signature;
    entry.graphics = graphics;
    entry.args.clear();
}
void set_pipeline(void *list, ID3D12PipelineState *pipeline) {
    state_of(list).pipeline = pipeline;
}
void clear_list(void *list) {
    {
        const std::lock_guard lock(lists_mutex);
        lists.erase(list);
    }
    free_ring(list);
}
const Layout *layout_of(void *list) {
    const auto &entry = state_of(list);
    const std::lock_guard lock(layouts_mutex);
    const auto found = layouts.find(entry.root);
    return found == layouts.end() ? nullptr : &found->second;
}
void write_at(void *list, UINT parameter, std::size_t offset, const void *data,
              std::size_t bytes) {
    const auto *layout = layout_of(list);
    if (!layout || parameter >= layout->params.size())
        return;
    const auto at = layout->params[parameter].at + offset;
    if (at + bytes > layout->params[parameter].at + layout->params[parameter].bytes)
        return;
    auto &entry = state_of(list);
    if (entry.args.size() < at + bytes)
        entry.args.resize(at + bytes);
    std::memcpy(entry.args.data() + at, data, bytes);
}
void write_constant(void *list, UINT parameter, UINT offset, UINT count, const void *data,
                    bool graphics) {
    if (!data || graphics != state_of(list).graphics)
        return;
    write_at(list, parameter, std::size_t{4} * offset, data, std::size_t{4} * count);
}
void write_view(void *list, UINT parameter, std::uint64_t address, bool graphics) {
    if (graphics != state_of(list).graphics)
        return;
    write_at(list, parameter, 0, &address, sizeof address);
}
void write_table(void *list, UINT parameter, std::uint64_t address, bool graphics) {
    if (graphics != state_of(list).graphics)
        return;
    const auto low = static_cast<std::uint32_t>(address);
    write_at(list, parameter, 0, &low, sizeof low);
}

Binding before_draw(void *list, ID3D12GraphicsCommandList *host) {
    if (!host || !uma_on_gpu())
        return Binding::untranslated;
    bool graphics = false;
    ID3D12RootSignature *abi = nullptr;
    ID3D12PipelineState *pipeline = nullptr;
    {
        const auto &entry = state_of(list);
        if (!entry.pipeline || !translated_pipeline(entry.pipeline, graphics))
            return Binding::untranslated;
        const std::lock_guard lock(pipelines_mutex);
        const auto found = pipelines.find(entry.pipeline);
        if (found == pipelines.end())
            return Binding::untranslated;
        abi = found->second.signature;
        pipeline = entry.pipeline;
    }
    if (!abi || !pipeline)
        return Binding::untranslated;
    const auto layout = layout_of(list);
    const auto &args = state_of(list).args;
    const auto inline_bytes =
        layout ? std::min(graphics ? layout->graphics_inline : layout->compute_inline,
                          args.size())
               : 0;
    if (trace_draws())
        trace_arguments(args, args.size());
    std::vector<std::uint8_t> table(args.begin() + inline_bytes, args.end());
    if (table.size() < 16)
        table.resize(16);
    std::vector<std::uint32_t> io(kernel_sgprs);
    const auto table_console = place_in_ring(list, table.data(), table.size());
    if (!table_console)
        return Binding::skipped;
    io[0] = static_cast<std::uint32_t>(table_console);
    io[1] = static_cast<std::uint32_t>(table_console >> 32);
    if (inline_bytes)
        std::memcpy(io.data() + (graphics ? 8 : 4), args.data(), inline_bytes);
    const auto io_console = place_in_ring(list, io.data(), io.size() * sizeof(std::uint32_t));
    static const std::uint8_t zeroes[256]{};
    const auto res_console = place_in_ring(list, zeroes, sizeof zeroes);
    if (!io_console || !res_console)
        return Binding::skipped;
    const auto address_of = [](std::uintptr_t console) {
        return uma_host_address(console);
    };
    const std::uint64_t views[6] = {address_of(io_console), address_of(res_console),
                                    uma_window_va(0),       uma_window_va(1),
                                    uma_window_va(2),       uma_window_va(3)};
    if (graphics) {
        host->SetGraphicsRootSignature(abi);
        for (UINT k = 0; k < 6; ++k)
            host->SetGraphicsRootUnorderedAccessView(k, views[k]);
    } else {
        host->SetComputeRootSignature(abi);
        for (UINT k = 0; k < 6; ++k)
            host->SetComputeRootUnorderedAccessView(k, views[k]);
    }
    host->SetPipelineState(pipeline);
    return Binding::bound;
}
} // namespace xv::runtime::modules::d3d12x::xcl
