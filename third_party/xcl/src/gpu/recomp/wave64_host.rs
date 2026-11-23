//! Dataflow facts used to run a wave64 program on two native wave32 halves.
//!
//! The console's `v_readlane_b32 s, v, 32+k` normally needs the upper half of the wave,
//! which does not exist in a 32-lane host subgroup. Two local rewrites cover the cases
//! measured in the xsx_layer study: an idempotent reduction (`min`/`max`/`and`/`or`)
//! folds each half separately and reads `k` instead of `32+k`; a wave-uniform value is
//! the same as the invocation's own value. Everything else is either exchanged through
//! groupshared memory (compute/mesh/geometry) or refused.
//!
//! The analysis is a forward pass over instructions. It is deliberately conservative:
//! a VGPR is only given a special kind when the defining instruction matches a local
//! reduction/broadcast pattern; every other write makes it `Other`.

use std::collections::BTreeSet;

use super::super::rdna2::{self, Variant};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Op {
    Min,
    Max,
    And,
    Or,
}

impl Op {
    /// Idempotent operation of an idempotent ALU mnemonic.
    fn of(name: &str) -> Option<Self> {
        let lower = name.to_ascii_lowercase();
        let n = lower.strip_prefix('v')?.trim_start_matches('_');
        [
            ("min_u32", Op::Min),
            ("max_u32", Op::Max),
            ("min_i32", Op::Min),
            ("max_i32", Op::Max),
            ("and_b32", Op::And),
            ("or_b32", Op::Or),
        ]
        .into_iter()
        .find(|(m, _)| n.starts_with(m))
        .map(|(_, op)| op)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Val {
    Other,
    /// Same value in all 64 lanes (SGPR broadcast / reduction folded through an SGPR).
    Uniform,
    /// Same idempotent reduction of each 32-lane half, held by every lane of that half.
    HalfReduce(Op),
}

/// Per-instruction decisions for the emitter and hazard detectors.
#[derive(Debug, Clone, Default)]
pub struct HostWaveMap {
    /// `v_readlane` PCs whose source is an idempotent reduction; lane `32+k` may read `k`.
    pub idempotent: BTreeSet<u64>,
    /// `v_readlane` PCs whose source is wave-uniform; the read is the invocation's own value.
    pub uniform: BTreeSet<u64>,
    /// Cross-lane ops that can be exchanged through groupshared memory.
    pub exchange: BTreeSet<u64>,
    /// DPP / permlane instructions that form a recognized idempotent reduction chain.
    pub reduction_chain: BTreeSet<u64>,
}

impl HostWaveMap {
    pub fn is_rewritten(&self, pc: u64) -> bool {
        self.idempotent.contains(&pc) || self.uniform.contains(&pc)
    }
    pub fn is_empty(&self) -> bool {
        self.idempotent.is_empty() && self.uniform.is_empty() && self.exchange.is_empty() && self.reduction_chain.is_empty()
    }
}

/// Build the per-PC decisions for `code`.
pub fn analyze(code: &[u8]) -> HostWaveMap {
    let mut map = HostWaveMap::default();
    let mut vgpr = vec![Val::Other; 256];
    // Scalar registers are uniform across the wave unless they carry a 64-bit mask.
    let mut sgpr = vec![Val::Uniform; 106];
    let mut off = 0usize;
    while off + 4 <= code.len() {
        let Ok(d) = rdna2::decode(&code[off..], off as u64) else { break };
        off += d.len;
        let n = d.inst.name;
        let (vr, vw) = rdna2::vgpr_uses(&d);
        let (sr, sw) = rdna2::sgpr_uses(&d);
        let kind_v = |r: u64| vgpr.get(r as usize).copied().unwrap_or(Val::Other);
        let kind_s = |r: u64| if r < 106 { sgpr[r as usize] } else { Val::Other };
        let mut res = Val::Other;
        let mut sres: Option<(u64, Val)> = None;

        match n {
            "V_READFIRSTLANE_B32" => {
                // An SGPR is scalar; its value is the same in every lane of the wave.
                sres = sw.first().copied().map(|r| (r, Val::Uniform));
            }
            "V_READLANE_B32" => {
                let src = kind_v(vr.first().copied().unwrap_or(0));
                let lane = rdna2::field_value(d.enc, &d.words, "SRC1").unwrap_or(0);
                let known_lane = matches!(lane, 160..=192 | 255) || d.literal.is_some();
                match src {
                    Val::Uniform => {
                        map.uniform.insert(d.pc);
                    }
                    Val::HalfReduce(_) if known_lane => {
                        map.idempotent.insert(d.pc);
                    }
                    _ => {
                        // Any other cross-half read can be exchanged through groupshared
                        // memory in a stage with a workgroup (compute / mesh / geometry).
                        // An SGPR lane is fine too: a mask-derived lane is the full
                        // wave64 value after the ballot synchronization, so both halves
                        // request the same lane.
                        map.exchange.insert(d.pc);
                    }
                }
                sres = sw.first().copied().map(|r| (r, Val::Uniform));
            }
            "DS_BPERMUTE_B32" | "DS_PERMUTE_B32" => {
                // A workgroup-backed stage can exchange the 64 lanes through LDS.
                map.exchange.insert(d.pc);
            }
            "V_WRITELANE_B32" => {
                res = kind_v(vr.first().copied().unwrap_or(0));
                map.exchange.insert(d.pc);
            }
            "V_MOV_B32" | "V_MOVDX_B32" => {
                if let Some(&r) = vr.first() {
                    res = if r < 106 { kind_s(r) } else { kind_v(r) };
                }
            }
            "V_PERMLANEX16_B32" | "V_PERMLANE16_B32" => {
                if let Some(&r) = vr.first() {
                    res = kind_v(r);
                }
            }
            "S_MOV_B32" | "S_CSELECT_B32" => {
                let kind = vr.first().copied().map(&kind_v).or_else(|| sr.first().copied().map(&kind_s)).unwrap_or(Val::Uniform);
                sres = sw.first().copied().map(|r| (r, kind));
            }
            _ => {
                if let Some(op) = Op::of(n) {
                    let srcs: Vec<Val> = vr.iter().copied().map(&kind_v).collect();
                    let ssrcs: Vec<Val> = sr.iter().copied().map(&kind_s).collect();
                    let dpp = matches!(d.variant, Variant::Dpp8 | Variant::Dpp16);
                    let all_uniform = !srcs.is_empty() && srcs.iter().all(|k| *k == Val::Uniform) && ssrcs.iter().all(|k| *k == Val::Uniform);
                    let self_reduce = dpp && srcs.len() >= 2 && srcs[0] == srcs[1];
                    let permute_reduce = !dpp
                        && srcs.len() >= 2
                        && srcs[0] == srcs[1]
                        && matches!(srcs[0], Val::HalfReduce(_))
                        && ssrcs.is_empty();
                    let fold = !dpp
                        && srcs.len() >= 2
                        && ssrcs.iter().any(|k| *k == Val::Uniform)
                        && matches!(srcs[0], Val::HalfReduce(_));
                    if all_uniform {
                        res = Val::Uniform;
                    } else if self_reduce || permute_reduce {
                        res = Val::HalfReduce(op);
                        map.reduction_chain.insert(d.pc);
                    } else if fold {
                        res = Val::Uniform;
                    }
                } else if !vr.is_empty() && vr.iter().all(|r| kind_v(*r) == Val::Uniform) && sr.iter().all(|r| kind_s(*r) == Val::Uniform) {
                    // Purely scalar arithmetic stays uniform.
                    res = Val::Uniform;
                }
            }
        }

        if let Some(&w) = vw.first() {
            if (w as usize) < vgpr.len() {
                vgpr[w as usize] = res;
            }
        }
        if let Some((r, kind)) = sres {
            if (r as usize) < sgpr.len() {
                sgpr[r as usize] = kind;
            }
        }
        // Writes to saved-EXEC registers are half-local masks, not wave-uniform.
        if n.starts_with("S_") && n.ends_with("EXEC_B64") {
            for &r in &sw {
                if (r as usize) < sgpr.len() {
                    sgpr[r as usize] = Val::Other;
                }
            }
        }
    }
    map
}
