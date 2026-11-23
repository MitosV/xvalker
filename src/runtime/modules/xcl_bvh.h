/* The console's acceleration structures: the RDNA2 nodes `image_bvh(64)_intersect_ray` reads.
 *
 * A structure is a 256-byte header (the root node id at 0x30) followed by the nodes, 64-byte
 * aligned; a node id is (its offset from the start of the structure / 8) | its kind. The
 * kinds are 0-3 (a pair of triangles), 4 (a box of half floats), 5 (a box of floats) and 6
 * (an instance).
 *
 * The functions are the ones of `xcl_bvh.dll` (the crate `tools/xcl-bvh`), which is XCL's
 * module for the format, vendored byte for byte.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a call says. */
#define XCL_BVH_OK 0        /* the structure is whole */
#define XCL_BVH_E_ARGS (-1) /* a pointer was null or a count was wrong */
#define XCL_BVH_E_FORMAT (-2) /* what was given is too small to be a structure */
#define XCL_BVH_E_BROKEN (-3) /* a node is not there, or is of a kind there is none of */

/* The kinds of node the console's format has. */
#define XCL_BVH_NODE_TRIANGLE 0
#define XCL_BVH_NODE_BOX16 4
#define XCL_BVH_NODE_BOX32 5
#define XCL_BVH_NODE_INSTANCE 6

/* What a structure holds. */
typedef struct XclBvhInfo {
    uint32_t root;      /* the root node id */
    uint32_t nodes;     /* how many nodes the walk found */
    uint32_t boxes;     /* how many of them are boxes */
    uint32_t triangles; /* how many of them are triangles */
    uint32_t instances; /* how many of them are instances */
    uint32_t error;     /* 0 when the structure is whole */
    float bounds[6];    /* min x y z, max x y z */
} XclBvhInfo;

/* What a traversal found. */
typedef struct XclBvhHit {
    float t;           /* how far the hit is along the ray */
    uint32_t primitive; /* the triangle inside its geometry */
    uint32_t geometry;  /* the geometry the triangle belongs to */
    uint32_t instance;  /* the instance the geometry is in, 0 in a bottom level */
    uint32_t hit;       /* 0 when the ray missed */
} XclBvhHit;

/* A bottom level a top level points at. */
typedef struct XclBvhBlas {
    uint64_t address;    /* the address the instance names it by */
    const uint8_t *data; /* the structure itself */
    size_t size;         /* how long it is */
} XclBvhBlas;

/* Whether the structure is one the console's shaders can read, and what it holds.
 * `info` may be null. */
int xcl_bvh_validate(const uint8_t *data, size_t size, XclBvhInfo *info);

/* The closest hit in a bottom level: the distance along the ray, the triangle inside its
 * geometry and the geometry itself. `hit` may not be null. */
int xcl_bvh_trace(const uint8_t *data, size_t size, const float origin[3],
                  const float direction[3], float tmax, XclBvhHit *hit);

/* The closest hit through a top level: the instance the hit is in, the triangle inside its
 * geometry and the geometry itself. */
int xcl_bvh_trace_scene(const uint8_t *tlas, size_t tlas_size, const XclBvhBlas *blases,
                        size_t blas_count, const float origin[3], const float direction[3],
                        float tmax, XclBvhHit *hit);

#ifdef __cplusplus
}
#endif
