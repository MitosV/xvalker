//! The console's acceleration structures: the RDNA2 nodes `image_bvh(64)_intersect_ray`
//! reads, as XCL's module describes them (vendored byte for byte, see
//! `third_party/xcl/README-bvh.md`), with a C interface for the runtime, which has to read
//! what the title's own builder makes.
//!
//! The interface answers two questions about a structure: whether it is one the console's
//! shaders can read (`xcl_bvh_validate`), and what a ray finds in it (`xcl_bvh_trace`,
//! `xcl_bvh_trace_scene`).

// the vendored module has more than the interface needs
#![allow(dead_code)]

#[path = "../../../third_party/xcl/src/gpu/bvh.rs"]
mod bvh;

use std::slice;

/// What the vendored module offers, for the tools that make structures to read.
pub use bvh::{build, build_top, triangle_hit, Aabb, Bvh, Instance, Triangle};

/// The structure is one the console's shaders can read.
pub const XCL_BVH_OK: i32 = 0;
/// A pointer was null or a count was wrong.
pub const XCL_BVH_E_ARGS: i32 = -1;
/// What was given is too small to be a structure.
pub const XCL_BVH_E_FORMAT: i32 = -2;
/// The structure is broken: a node is not there, or is of a kind there is none of.
pub const XCL_BVH_E_BROKEN: i32 = -3;

/// The kinds of node the console's format has.
pub const XCL_BVH_NODE_TRIANGLE: u32 = 0;
pub const XCL_BVH_NODE_BOX16: u32 = 4;
pub const XCL_BVH_NODE_BOX32: u32 = 5;
pub const XCL_BVH_NODE_INSTANCE: u32 = 6;

/// What a structure holds.
#[repr(C)]
pub struct XclBvhInfo {
    /// The root node id.
    pub root: u32,
    /// How many nodes the walk found.
    pub nodes: u32,
    /// How many of them are boxes.
    pub boxes: u32,
    /// How many of them are triangles.
    pub triangles: u32,
    /// How many of them are instances.
    pub instances: u32,
    /// 0 when the structure is whole, the reason otherwise.
    pub error: u32,
    /// The bounds of everything below the root: min x y z, max x y z.
    pub bounds: [f32; 6],
}

/// What a traversal found.
#[repr(C)]
pub struct XclBvhHit {
    /// How far the hit is along the ray.
    pub t: f32,
    /// The triangle inside its geometry.
    pub primitive: u32,
    /// The geometry the triangle belongs to.
    pub geometry: u32,
    /// The instance the geometry is in, 0 in a bottom level.
    pub instance: u32,
    /// 0 when the ray missed.
    pub hit: u32,
}

/// A bottom level a top level points at.
#[repr(C)]
pub struct XclBvhBlas {
    /// The address the instance names it by.
    pub address: u64,
    /// The structure itself.
    pub data: *const u8,
    /// How long it is.
    pub size: usize,
}

fn word(data: &[u8], at: usize) -> u32 {
    if at + 4 > data.len() {
        return 0;
    }
    u32::from_le_bytes(data[at..at + 4].try_into().unwrap())
}
fn node_at(node: u32) -> usize {
    ((node & !7) as usize) << 3
}
/// How long a node of a kind is.
fn node_bytes(kind: u32) -> usize {
    match kind {
        0..=3 => 64,
        XCL_BVH_NODE_BOX16 => 64,
        XCL_BVH_NODE_BOX32 | XCL_BVH_NODE_INSTANCE => 128,
        _ => 0,
    }
}
/// The bytes of a pointer, or nothing when there is none.
fn bytes_of<'a>(data: *const u8, size: usize) -> Option<&'a [u8]> {
    if data.is_null() || size == 0 {
        return None;
    }
    Some(unsafe { slice::from_raw_parts(data, size) })
}
fn vec3(data: &[u8], at: usize) -> [f32; 3] {
    [0, 1, 2].map(|k| f32::from_bits(word(data, at + 4 * k)))
}

/// The nodes of a structure, in the order the walk finds them.
fn walk(data: &[u8], root: u32) -> Result<Vec<u32>, i32> {
    let mut found = Vec::new();
    let mut stack = vec![root];
    while let Some(node) = stack.pop() {
        if node == bvh::INVALID {
            continue;
        }
        let kind = node & 7;
        if node_bytes(kind) == 0 {
            return Err(XCL_BVH_E_BROKEN);
        }
        let at = node_at(node);
        if at + node_bytes(kind) > data.len() {
            return Err(XCL_BVH_E_BROKEN);
        }
        // a structure that points at itself would walk forever
        if found.len() > data.len() / 64 {
            return Err(XCL_BVH_E_BROKEN);
        }
        found.push(node);
        if kind == XCL_BVH_NODE_BOX16 || kind == XCL_BVH_NODE_BOX32 {
            for k in 0..4 {
                let child = word(data, at + 4 * k);
                if child == bvh::INVALID {
                    continue;
                }
                if node_bytes(child & 7) == 0 || node_at(child) + node_bytes(child & 7) > data.len()
                {
                    return Err(XCL_BVH_E_BROKEN);
                }
                stack.push(child);
            }
        }
    }
    Ok(found)
}

/// The triangles of a structure, by node id.
fn triangles(data: &[u8], root: u32) -> Vec<u32> {
    walk(data, root)
        .unwrap_or_default()
        .into_iter()
        .filter(|&node| node & 7 <= 3)
        .collect()
}

/// The geometry a hit is in: the triangle the traversal stopped at.
fn geometry_of(
    data: &[u8],
    nodes: &[u32],
    primitive: u32,
    t: f32,
    origin: [f32; 3],
    direction: [f32; 3],
    tmax: f32,
) -> u32 {
    let inverse = [1.0 / direction[0], 1.0 / direction[1], 1.0 / direction[2]];
    for &node in nodes {
        let at = node_at(node);
        let index = word(data, at + if node & 7 == 1 { 56 } else { 52 });
        if index != primitive {
            continue;
        }
        let hit = bvh::intersect_node(data, node, tmax, origin, direction, inverse, true)
            .map(f32::from_bits);
        if hit[0] == f32::INFINITY {
            continue;
        }
        let found = hit[0] / hit[1];
        if (found - t).abs() <= 1e-4 * found.abs().max(1.0) {
            return (word(data, at + 48) >> 8) & 0xFF_FFFF;
        }
    }
    u32::MAX
}

/// Whether the structure is one the console's shaders can read, and what it holds.
#[no_mangle]
pub extern "C" fn xcl_bvh_validate(data: *const u8, size: usize, info: *mut XclBvhInfo) -> i32 {
    if info.is_null() {
        return XCL_BVH_E_ARGS;
    }
    let info = unsafe { &mut *info };
    *info = XclBvhInfo {
        root: bvh::INVALID,
        nodes: 0,
        boxes: 0,
        triangles: 0,
        instances: 0,
        error: XCL_BVH_OK as u32,
        bounds: [0.0; 6],
    };
    let Some(bytes) = bytes_of(data, size) else {
        info.error = XCL_BVH_E_ARGS as u32;
        return XCL_BVH_E_ARGS;
    };
    if bytes.len() < bvh::HEADER_BYTES {
        info.error = XCL_BVH_E_FORMAT as u32;
        return XCL_BVH_E_FORMAT;
    }
    let root = word(bytes, bvh::ROOT_AT);
    info.root = root;
    if root == bvh::INVALID {
        // an empty structure is whole: it holds nothing
        return XCL_BVH_OK;
    }
    let kind = root & 7;
    if kind != XCL_BVH_NODE_BOX32 && kind != XCL_BVH_NODE_BOX16 {
        info.error = XCL_BVH_E_BROKEN as u32;
        return XCL_BVH_E_BROKEN;
    }
    let found = match walk(bytes, root) {
        Ok(found) => found,
        Err(error) => {
            info.error = error as u32;
            return error;
        }
    };
    info.nodes = found.len() as u32;
    for &node in &found {
        match node & 7 {
            XCL_BVH_NODE_BOX16 | XCL_BVH_NODE_BOX32 => info.boxes += 1,
            XCL_BVH_NODE_INSTANCE => info.instances += 1,
            _ => info.triangles += 1,
        }
    }
    let structure = bvh::Bvh { data: bytes.to_vec(), root };
    let (min, max) = structure.bounds();
    info.bounds = [min[0], min[1], min[2], max[0], max[1], max[2]];
    XCL_BVH_OK
}

/// The closest hit in a bottom level.
#[no_mangle]
pub extern "C" fn xcl_bvh_trace(
    data: *const u8,
    size: usize,
    origin: *const f32,
    direction: *const f32,
    tmax: f32,
    hit: *mut XclBvhHit,
) -> i32 {
    if hit.is_null() || origin.is_null() || direction.is_null() {
        return XCL_BVH_E_ARGS;
    }
    let hit = unsafe { &mut *hit };
    *hit = XclBvhHit { t: 0.0, primitive: 0, geometry: 0, instance: 0, hit: 0 };
    let Some(bytes) = bytes_of(data, size) else {
        return XCL_BVH_E_ARGS;
    };
    if bytes.len() < bvh::HEADER_BYTES {
        return XCL_BVH_E_FORMAT;
    }
    let start = unsafe { slice::from_raw_parts(origin, 3) };
    let way = unsafe { slice::from_raw_parts(direction, 3) };
    let origin = [start[0], start[1], start[2]];
    let direction = [way[0], way[1], way[2]];
    let root = word(bytes, bvh::ROOT_AT);
    if root == bvh::INVALID {
        return XCL_BVH_OK;
    }
    let structure = bvh::Bvh { data: bytes.to_vec(), root };
    let Some((t, primitive)) = structure.trace(origin, direction, tmax) else {
        return XCL_BVH_OK;
    };
    let nodes = triangles(bytes, root);
    let geometry = geometry_of(bytes, &nodes, primitive, t, origin, direction, tmax);
    hit.t = t;
    hit.primitive = primitive;
    hit.geometry = geometry;
    hit.hit = 1;
    XCL_BVH_OK
}

/// The closest hit through a top level: the instance it is in, the geometry and the triangle.
#[no_mangle]
pub extern "C" fn xcl_bvh_trace_scene(
    tlas: *const u8,
    tlas_size: usize,
    blases: *const XclBvhBlas,
    blas_count: usize,
    origin: *const f32,
    direction: *const f32,
    tmax: f32,
    hit: *mut XclBvhHit,
) -> i32 {
    if hit.is_null() || origin.is_null() || direction.is_null() {
        return XCL_BVH_E_ARGS;
    }
    let hit = unsafe { &mut *hit };
    *hit = XclBvhHit { t: 0.0, primitive: 0, geometry: 0, instance: 0, hit: 0 };
    let Some(bytes) = bytes_of(tlas, tlas_size) else {
        return XCL_BVH_E_ARGS;
    };
    if bytes.len() < bvh::HEADER_BYTES {
        return XCL_BVH_E_FORMAT;
    }
    if blases.is_null() && blas_count != 0 {
        return XCL_BVH_E_ARGS;
    }
    let listed = if blas_count == 0 {
        &[][..]
    } else {
        unsafe { slice::from_raw_parts(blases, blas_count) }
    };
    let mut levels: Vec<(u64, Vec<u8>)> = Vec::with_capacity(listed.len());
    for one in listed {
        let Some(data) = bytes_of(one.data, one.size) else {
            return XCL_BVH_E_ARGS;
        };
        levels.push((one.address, data.to_vec()));
    }
    let start = unsafe { slice::from_raw_parts(origin, 3) };
    let way = unsafe { slice::from_raw_parts(direction, 3) };
    let origin = [start[0], start[1], start[2]];
    let direction = [way[0], way[1], way[2]];
    let root = word(bytes, bvh::ROOT_AT);
    if root == bvh::INVALID {
        return XCL_BVH_OK;
    }
    let structure = bvh::Bvh { data: bytes.to_vec(), root };
    let lookup = |address: u64| -> Option<&[u8]> {
        levels
            .iter()
            .find(|(at, _)| *at == address)
            .map(|(_, data)| data.as_slice())
    };
    let Some((t, primitive, instance)) = structure.trace_scene(&lookup, origin, direction, tmax)
    else {
        return XCL_BVH_OK;
    };
    // the instance the hit is in names the bottom level it is in
    let mut geometry = u32::MAX;
    let mut address = 0u64;
    for node in walk(bytes, root).unwrap_or_default() {
        if node & 7 != XCL_BVH_NODE_INSTANCE {
            continue;
        }
        let at = node_at(node);
        if word(bytes, at + 48) & 0xFF_FFFF != instance {
            continue;
        }
        address = (word(bytes, at + 60) as u64) << 8;
        break;
    }
    if let Some((_, level)) = levels.iter().find(|(at, _)| *at == address) {
        let root = word(level, bvh::ROOT_AT);
        let nodes = triangles(level, root);
        geometry = geometry_of(level, &nodes, primitive, t, origin, direction, tmax);
    }
    hit.t = t;
    hit.primitive = primitive;
    hit.geometry = geometry;
    hit.instance = instance;
    hit.hit = 1;
    XCL_BVH_OK
}
