//! The CPU reference of the console's acceleration structures: it builds a bottom level and a
//! top level over them, reads both through the C interface, and checks every ray against a
//! brute-force search over the same triangles.
//!
//!   cargo run --release --bin xcl-bvh-reference

use xcl_bvh::{
    build, build_top, triangle_hit, xcl_bvh_trace, xcl_bvh_trace_scene, xcl_bvh_validate, Bvh,
    Instance, Triangle, XclBvhBlas, XclBvhHit, XclBvhInfo,
};

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        self.0 >> 40
    }
    fn between(&mut self, low: f32, high: f32) -> f32 {
        low + (high - low) * (self.next() as f32 / (1u64 << 24) as f32)
    }
}

fn triangles(count: usize, seed: u64) -> Vec<Triangle> {
    let mut rng = Rng(seed);
    (0..count)
        .map(|_| {
            let center = [rng.between(-8.0, 8.0), rng.between(-8.0, 8.0), rng.between(-8.0, 8.0)];
            [0; 3].map(|_| [0, 1, 2].map(|axis| center[axis] + rng.between(-1.5, 1.5)))
        })
        .collect()
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn main() {
    let mut failures = 0;

    // a bottom level, and what the interface says about it
    let tris = triangles(300, 7);
    let blas = build(&tris, 3);
    let mut info = XclBvhInfo {
        root: 0,
        nodes: 0,
        boxes: 0,
        triangles: 0,
        instances: 0,
        error: 0,
        bounds: [0.0; 6],
    };
    let status = xcl_bvh_validate(blas.data.as_ptr(), blas.data.len(), &mut info);
    println!(
        "bottom level: {status} root {:#x} nodes {} boxes {} triangles {} bounds {:?}",
        info.root, info.nodes, info.boxes, info.triangles, info.bounds
    );
    if status != 0 || info.triangles != 300 || info.root & 7 != 5 {
        failures += 1;
        println!("FAIL the bottom level is not what was built");
    }

    // rays through the bottom level, against a brute-force search
    let mut rng = Rng(99);
    let mut hits = 0;
    let mut wrong = 0;
    for _ in 0..5000 {
        let origin = [rng.between(-12.0, 12.0), rng.between(-12.0, 12.0), rng.between(-12.0, 12.0)];
        let towards = [rng.between(-6.0, 6.0), rng.between(-6.0, 6.0), rng.between(-6.0, 6.0)];
        let direction = sub(towards, origin);
        let mut hit = XclBvhHit { t: 0.0, primitive: 0, geometry: 0, instance: 0, hit: 0 };
        let status = {
            xcl_bvh_trace(
                blas.data.as_ptr(),
                blas.data.len(),
                origin.as_ptr(),
                direction.as_ptr(),
                100.0,
                &mut hit,
            )
        };
        if status != 0 {
            failures += 1;
            continue;
        }
        let brute = tris
            .iter()
            .enumerate()
            .filter_map(|(index, triangle)| {
                triangle_hit(triangle, origin, direction).map(|h| (h[0] / h[1], index as u32))
            })
            .filter(|&(t, _)| (0.0..100.0).contains(&t))
            .min_by(|a, b| a.0.total_cmp(&b.0));
        match (hit.hit == 1, brute) {
            (true, Some((t, index))) => {
                hits += 1;
                if hit.primitive != index || hit.geometry != 3 || (hit.t - t).abs() > 1e-4 * t.max(1.0)
                {
                    wrong += 1;
                }
            }
            (false, None) => {}
            _ => wrong += 1,
        }
    }
    println!("bottom level: {hits} rays hit, {wrong} of them wrong");
    failures += wrong;

    // a top level over three bottom levels
    let levels: Vec<(Vec<Triangle>, Bvh)> = (0..3)
        .map(|index| {
            let tris: Vec<Triangle> = triangles(40 + 10 * index, 100 + index as u64)
                .into_iter()
                .map(|t| t.map(|v| v.map(|x| x * 0.25)))
                .collect();
            let built = build(&tris, 0);
            (tris, built)
        })
        .collect();
    let address = |index: usize| 0x10_0000u64 * (index as u64 + 1);
    let instances: Vec<Instance> = (0..24)
        .map(|index| {
            let level = index % 3;
            let move_to = [
                rng.between(-20.0, 20.0),
                rng.between(-20.0, 20.0),
                rng.between(-20.0, 20.0),
            ];
            let scale = [
                rng.between(0.5, 2.0),
                rng.between(0.5, 2.0),
                rng.between(0.5, 2.0),
            ];
            Instance {
                transform: [
                    [scale[0], 0.0, 0.0, move_to[0]],
                    [0.0, scale[1], 0.0, move_to[1]],
                    [0.0, 0.0, scale[2], move_to[2]],
                ],
                id: 1000 + index as u32,
                mask: 0xFF,
                contribution: 0,
                flags: 0,
                blas_address: address(level),
                blas_root: levels[level].1.root,
                blas_bounds: levels[level].1.bounds(),
            }
        })
        .collect();
    let top = build_top(&instances);
    let mut info = XclBvhInfo {
        root: 0,
        nodes: 0,
        boxes: 0,
        triangles: 0,
        instances: 0,
        error: 0,
        bounds: [0.0; 6],
    };
    let status = xcl_bvh_validate(top.data.as_ptr(), top.data.len(), &mut info);
    println!(
        "top level: {status} root {:#x} nodes {} boxes {} instances {}",
        info.root, info.nodes, info.boxes, info.instances
    );
    if status != 0 || info.instances != 24 {
        failures += 1;
        println!("FAIL the top level is not what was built");
    }

    let listed: Vec<XclBvhBlas> = (0..3)
        .map(|index| XclBvhBlas {
            address: address(index),
            data: levels[index].1.data.as_ptr(),
            size: levels[index].1.data.len(),
        })
        .collect();
    let mut hits = 0;
    let mut wrong = 0;
    for _ in 0..5000 {
        let origin = [rng.between(-30.0, 30.0), rng.between(-30.0, 30.0), rng.between(-30.0, 30.0)];
        let towards = [rng.between(-20.0, 20.0), rng.between(-20.0, 20.0), rng.between(-20.0, 20.0)];
        let direction = sub(towards, origin);
        let mut hit = XclBvhHit { t: 0.0, primitive: 0, geometry: 0, instance: 0, hit: 0 };
        let status = {
            xcl_bvh_trace_scene(
                top.data.as_ptr(),
                top.data.len(),
                listed.as_ptr(),
                listed.len(),
                origin.as_ptr(),
                direction.as_ptr(),
                100.0,
                &mut hit,
            )
        };
        if status != 0 {
            failures += 1;
            continue;
        }
        let brute = instances
            .iter()
            .flat_map(|instance| {
                let level = (0..3).find(|&k| address(k) == instance.blas_address).unwrap();
                levels[level].0.iter().enumerate().filter_map(move |(index, triangle)| {
                    let world = triangle.map(|v| {
                        [
                            instance.transform[0][0] * v[0]
                                + instance.transform[0][1] * v[1]
                                + instance.transform[0][2] * v[2]
                                + instance.transform[0][3],
                            instance.transform[1][0] * v[0]
                                + instance.transform[1][1] * v[1]
                                + instance.transform[1][2] * v[2]
                                + instance.transform[1][3],
                            instance.transform[2][0] * v[0]
                                + instance.transform[2][1] * v[1]
                                + instance.transform[2][2] * v[2]
                                + instance.transform[2][3],
                        ]
                    });
                    triangle_hit(&world, origin, direction)
                        .map(|h| (h[0] / h[1], index as u32, instance.id))
                })
            })
            .filter(|h| (0.0..100.0).contains(&h.0))
            .min_by(|a, b| a.0.total_cmp(&b.0));
        match (hit.hit == 1, brute) {
            (true, Some((t, index, id))) => {
                hits += 1;
                if hit.primitive != index || hit.instance != id || (hit.t - t).abs() > 1e-3 * t.max(1.0)
                {
                    wrong += 1;
                }
            }
            (false, None) => {}
            _ => wrong += 1,
        }
    }
    println!("top level: {hits} rays hit, {wrong} of them wrong");
    failures += wrong;

    if failures == 0 {
        println!("the CPU reference agrees with the brute-force search on every ray");
    } else {
        println!("FAIL {failures} checks");
        std::process::exit(1);
    }
}
