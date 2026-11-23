#pragma once
#include <cstddef>
#include <cstdint>

// The BCPack decoder, in bcpack.dll (tools/bcpack). It decodes one BCPack
// chunk into whole BCn blocks.
//
// `mode` is the console's DSTORAGE_BCPACK_MODE: 1 = BC1, 3 = BC3, 4 = BC4,
// 5 = BC5. `dst_size` is the chunk's decoded byte count and must be a whole
// number of blocks (8 bytes for BC1 and BC4, 16 for BC3 and BC5). The call
// returns S_OK, E_DSTORAGE_INVALID_DESTINATION_SIZE (0x8924000F) or
// E_DSTORAGE_DECOMPRESSION_ERROR (0x89240030), and writes nothing to the
// destination when it fails.
//
// The runtime does not link against the DLL: it loads it on the first
// compressed request and takes the address of this function from it.
extern "C" int32_t xv_bcpack_decode(uint32_t mode, const uint8_t *src, size_t src_size,
                                    uint8_t *dst, size_t dst_size);

// What GetProcAddress gives for it.
using XvBcpackDecode = int32_t (*)(uint32_t, const uint8_t *, size_t, uint8_t *, size_t);
