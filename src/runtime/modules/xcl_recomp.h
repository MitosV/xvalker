/* XCL's console shader recompiler (RDNA2 -> HLSL), C ABI.
 *
 * The library translates console shader containers (the Xbox Series X|S shader
 * blobs found in a shader pack) into HLSL that DXC can compile. It is a pure
 * translator: no GPU, no Windows, no runtime.
 *
 *   XclResult result;
 *   if (xcl_translate(container, container_size, XCL_FLAG_NONE, &result) == 0) {
 *       // result.hlsl holds the HLSL (UTF-8, NUL-terminated)
 *       // result.profiles holds the DXC profile(s), comma separated
 *       xcl_free(&result);
 *   } else {
 *       // result.error holds the error text (UTF-8, NUL-terminated)
 *       xcl_free(&result);
 *   }
 *
 * The strings of a result belong to the result: xcl_free() releases them. Calling
 * xcl_free() twice on the same result is safe (the pointers are cleared).
 */

#ifndef XVALKER_XCL_RECOMP_H
#define XVALKER_XCL_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for xcl_translate(). */
#define XCL_FLAG_NONE 0u
/* Force wave64 semantics even when the program looks like a wave32 program. */
#define XCL_FLAG_FORCE_WAVE64 (1u << 0)
/* Force wave32 semantics even when the program looks like a wave64 program. */
#define XCL_FLAG_FORCE_WAVE32 (1u << 1)

/* The result of a translation. Every pointer is owned by the result and released by
 * xcl_free(). */
typedef struct XclResult {
    /* The translated HLSL (UTF-8, NUL-terminated). NULL on failure. */
    char* hlsl;
    /* The DXC profile(s) the HLSL needs, comma separated (e.g. "vs_6_6"). NULL on
     * failure. */
    char* profiles;
    /* The error text (UTF-8, NUL-terminated). NULL on success. */
    char* error;
    /* The number of instructions of the program. */
    uint64_t instructions;
    /* The number of instructions the translator could not translate. */
    uint64_t unsupported;
} XclResult;

/* Translate one console shader container into HLSL.
 *
 * container/size: the console container (the whole blob, not a program).
 * flags: XCL_FLAG_*.
 * out: the result; it is filled even when the translation fails.
 *
 * Returns 0 on success and a negative value on failure:
 *   -1  the arguments are wrong (a null container, no out)
 *   -2  the container is not a console shader container
 *   -3  no program of the container could be translated (out->error says why)
 */
int32_t xcl_translate(const uint8_t* container, size_t size, uint32_t flags, XclResult* out);

/* Release the strings of a result. Safe with a null pointer. */
void xcl_free(XclResult* result);

#ifdef __cplusplus
}
#endif

#endif /* XVALKER_XCL_RECOMP_H */
