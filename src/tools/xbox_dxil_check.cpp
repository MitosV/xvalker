// Lowers the DXIL of console shader containers the way the runtime does, then asks the host's
// signer and Direct3D 12 to take the result. Used with the shaders of the console's BVH builder,
// extracted from its DLL; the tool needs dxcompiler.dll and dxil.dll next to it.
//
// usage: xbox_dxil_check <folder with shader_NN.dxil> [dump folder]
#include "runtime/modules/xbox_dxil.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using xv::runtime::modules::lower_xbox_dxil;

namespace {
std::vector<std::uint8_t> read(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <shader folder> [dump folder]\n", argv[0]);
        return 2;
    }
    const fs::path source = argv[1];
    const fs::path dump = argc > 2 ? fs::path(argv[2]) : fs::path{};
    if (!dump.empty())
        fs::create_directories(dump);

    ComPtr<ID3D12Device> device;
    const auto made = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    std::printf("device: 0x%08lx\n", static_cast<unsigned long>(made));

    std::vector<fs::path> shaders;
    for (const auto &entry : fs::directory_iterator(source))
        if (entry.is_regular_file() && entry.path().extension() == ".dxil")
            shaders.push_back(entry.path());
    std::sort(shaders.begin(), shaders.end());

    int found = 0, lowered = 0, signed_ok = 0, pso = 0;
    for (const auto &path : shaders) {
        const auto bytes = read(path);
        ++found;
        std::string why;
        const auto result = lower_xbox_dxil(bytes, &why);
        if (!result) {
            std::printf("%s: not lowered: %s\n", path.filename().string().c_str(), why.c_str());
            continue;
        }
        ++lowered;
        if (result->signed_ok)
            ++signed_ok;
        if (!dump.empty())
            write(dump / path.filename(), result->dxil);
        ComPtr<ID3D12RootSignature> signature;
        if (FAILED(device->CreateRootSignature(0, bytes.data(), bytes.size(),
                                               IID_PPV_ARGS(&signature)))) {
            std::printf("%s: no root signature\n", path.filename().string().c_str());
            continue;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = signature.Get();
        desc.CS = {result->dxil.data(), result->dxil.size()};
        ComPtr<ID3D12PipelineState> state;
        if (SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state)))) {
            ++pso;
        } else {
            std::printf("%s: no pipeline state\n", path.filename().string().c_str());
        }
    }
    std::printf("found %d, lowered %d, signed %d, pipeline states %d\n", found, lowered,
                signed_ok, pso);
    return lowered == 0 || signed_ok != lowered || pso == 0 ? 1 : 0;
}
