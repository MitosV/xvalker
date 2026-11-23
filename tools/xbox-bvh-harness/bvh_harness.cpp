// Guest harness: loads the console's XboxBvhBuilder.dll through xvalker's title loader and
// asks it for the prebuild size of a two-triangle BLAS. Built with mingw and run as the title
// EXE of an xvboot package so LoadLibrary/GetProcAddress are the runtime's HLE versions.
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>

namespace {
// A static import of a Scarlett module makes xvalker's import analysis select its registry:
// the harness is a test title, not a PC program.
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(void *, const UINT *, REFIID,
                                                                     void **);
using Ctor = void *(__fastcall *)(void *, void *, bool);
using Dtor = void(__fastcall *)(void *);
using Prebuild = void(__fastcall *)(void *,
                                    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *,
                                    const void *, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *);

void log(const char *message) {
    std::fprintf(stderr, "bvh-harness: %s\n", message);
    std::fflush(stderr);
}

HMODULE module(const char *name) {
    const auto handle = LoadLibraryA(name);
    std::fprintf(stderr, "bvh-harness: LoadLibraryA(%s) = %p\n", name,
                 reinterpret_cast<void *>(handle));
    std::fflush(stderr);
    return handle;
}

template <class Function> Function symbol(HMODULE library, const char *name) {
    const auto address = GetProcAddress(library, name);
    std::fprintf(stderr, "bvh-harness: GetProcAddress(%s) = %p\n", name,
                 reinterpret_cast<void *>(address));
    std::fflush(stderr);
    return reinterpret_cast<Function>(address);
}

template <class Function> Function ordinal(HMODULE library, int number) {
    const auto address = GetProcAddress(library, reinterpret_cast<const char *>(number));
    std::fprintf(stderr, "bvh-harness: GetProcAddress(#%d) = %p\n", number,
                 reinterpret_cast<void *>(address));
    std::fflush(stderr);
    return reinterpret_cast<Function>(address);
}
} // namespace

int main() {
    log("start");
    const auto xs = module("d3d12_xs.dll");
    if (!xs)
        return 2;
    const auto iid = reinterpret_cast<const GUID *>(GetProcAddress(xs, "IID_ID3D12Device"));
    if (!xs || !iid)
        return 3;
    void *device = nullptr;
    const HRESULT hr = D3D12XboxCreateDevice(nullptr, nullptr, *iid, &device);
    std::fprintf(stderr, "bvh-harness: D3D12XboxCreateDevice = 0x%08lx, device %p\n",
                 static_cast<unsigned long>(hr), device);
    std::fflush(stderr);
    if (FAILED(hr) || !device)
        return 4;

    const auto builder = module("XboxBvhBuilder.dll");
    if (!builder)
        return 5;
    const auto ctor = ordinal<Ctor>(builder, 1);
    const auto dtor = ordinal<Dtor>(builder, 2);
    const auto prebuild = ordinal<Prebuild>(builder, 7);
    if (!ctor || !prebuild)
        return 6;

    void *object = new char[1 << 20]{};
    log("constructing builder");
    ctor(object, device, false);
    log("constructed");

    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = 6;
    geometry.Triangles.VertexBuffer.StrideInBytes = 12;
    geometry.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    log("asking for prebuild size");
    prebuild(object, &inputs, nullptr, &info);
    std::fprintf(stderr,
                 "bvh-harness: BLAS prebuild result %llu scratch %llu update %llu\n",
                 static_cast<unsigned long long>(info.ResultDataMaxSizeInBytes),
                 static_cast<unsigned long long>(info.ScratchDataSizeInBytes),
                 static_cast<unsigned long long>(info.UpdateScratchDataSizeInBytes));
    std::fflush(stderr);
    if (dtor)
        dtor(object);
    delete[] static_cast<char *>(object);
    log("done");
    return 0;
}
