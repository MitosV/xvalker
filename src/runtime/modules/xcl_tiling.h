#pragma once
#include <stdint.h>

// xcl_tiling.dll (tools/xcl-tiling): the console's texture layout — XCL's GFX10 tiling and
// layout library, vendored under third_party/xcl — behind a C ABI. The runtime loads it
// lazily; without it, title-written textures keep today's behavior.
//
// A texture the title writes in its own memory (a scratch-mapped one, a placed one) lies
// there as the console lays it out: planes, mips from the smallest, the mip tail, slices.
// These functions compute that layout from the D3D12 description the title gave (with its
// Layout field: 1 linear, 0x100|mode a swizzle mode, 0/2 the library's choice) and detile
// the memory into tightly packed linear rows per subresource, for a D3D12 upload.
//
// All functions return 0 (S_OK) or an HRESULT. Handles are opaque; one per texture.
#ifdef __cplusplus
extern "C" {
#endif

typedef struct XclTiling XclTiling;

// ABI version of the DLL (1): checked before the rest is used
uint32_t xcl_tiling_version(void);

// desc: the console's D3D12_RESOURCE_DESC, 0x38 bytes. series_s: 0 = Series X pipes,
// 1 = Series S (only the XOR swizzle modes depend on it). 2D textures (arrays, cubes) with
// one sample and a texel plane; what the console's library does not model is refused.
int32_t xcl_tiling_create(const uint8_t *desc, uint32_t series_s, XclTiling **out);
void xcl_tiling_destroy(XclTiling *tiling);

// bytes the texture takes in the title's memory
uint64_t xcl_tiling_size(const XclTiling *tiling);
// texel subresources in D3D12 order (mip + slice * mips): array slices * mip levels
uint32_t xcl_tiling_subresources(const XclTiling *tiling);
// one subresource's linear shape: rows rows of row_bytes, tightly packed
int32_t xcl_tiling_subresource(const XclTiling *tiling, uint32_t index, uint32_t *rows,
                               uint32_t *row_bytes);
// detile subresource index of [mem, mem + mem_bytes) into out (rows * row_bytes of
// xcl_tiling_subresource). mem must be readable; bytes of a slice past mem_bytes read as 0.
int32_t xcl_tiling_detile(const XclTiling *tiling, const uint8_t *mem, uint64_t mem_bytes,
                          uint32_t index, uint8_t *out, uint64_t out_bytes);

#ifdef __cplusplus
}
#endif