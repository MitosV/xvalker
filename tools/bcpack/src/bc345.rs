// Data tables ported from Leonida's clean-room BCPack decoder
// (tools/bcpack_tables.py, GPL-3.0). Only the entries Leonida marks
// confirmed are used; provisional entries are deliberately absent.

pub const BC3_ALPHA_PAIR: [[u8; 2]; 64] = [
    [0, 0], [1, 1], [6, 6], [4, 4], [3, 3], [5, 5], [7, 7], [1, 6],
    [6, 1], [0, 1], [1, 0], [7, 1], [1, 7], [0, 3], [3, 0], [0, 2],
    [2, 0], [2, 2], [5, 4], [3, 4], [4, 3], [4, 5], [1, 5], [5, 1],
    [0, 4], [4, 0], [2, 3], [3, 2], [5, 3], [3, 5], [6, 5], [5, 6],
    [6, 7], [7, 6], [5, 7], [4, 6], [6, 4], [7, 5], [6, 0], [0, 6],
    [4, 2], [2, 4], [4, 1], [1, 4], [5, 0], [0, 5], [6, 3], [3, 6],
    [3, 1], [5, 2], [1, 3], [2, 5], [7, 4], [4, 7], [7, 3], [3, 7],
    [2, 6], [6, 2], [2, 1], [1, 2], [0, 7], [7, 0], [2, 7], [7, 2],
];

pub const BC3_COLOUR_QUADRANT: [u8; 256] = [
    0x55, 0xaa, 0xff, 0x00, 0x50, 0x05, 0xaf, 0xfa, 0xae, 0xea, 0xab, 0xba, 0xee, 0xbb, 0xfe, 0xbf,
    0xef, 0xfb, 0xa5, 0x5a, 0xf0, 0x11, 0x0f, 0x44, 0xf5, 0x5f, 0xa0, 0x0a, 0x8a, 0xa2, 0xa8, 0x2a,
    0x7f, 0xf7, 0x77, 0xdf, 0xfd, 0xdd, 0x66, 0x99, 0x88, 0xbe, 0xeb, 0x22, 0xcc, 0x33, 0x8f, 0xf8,
    0x57, 0x75, 0xd5, 0x5d, 0x2f, 0xf2, 0x7a, 0x85, 0xa7, 0xda, 0x58, 0x25, 0xad, 0x52, 0x3f, 0xf3,
    0xcf, 0xe8, 0xfc, 0x8e, 0x2b, 0x7e, 0x6a, 0xa6, 0x9a, 0x70, 0x07, 0x0d, 0xa9, 0x78, 0x80, 0x08,
    0x02, 0xe7, 0xb2, 0x20, 0xdb, 0xd0, 0x87, 0x5e, 0xbd, 0x7c, 0x2d, 0xe5, 0xc7, 0xec, 0xce, 0x5b,
    0xb3, 0xd2, 0x2e, 0xb5, 0x3b, 0xe0, 0xe6, 0x6e, 0x68, 0x8b, 0x9b, 0xe2, 0xb9, 0x0e, 0xb8, 0x0b,
    0x3d, 0xd3, 0x51, 0x86, 0x27, 0x45, 0x15, 0xde, 0x54, 0x8d, 0xb7, 0xed, 0x7b, 0xb0, 0xd8, 0x29,
    0x72, 0x92, 0x7d, 0xd7, 0x35, 0xa3, 0xc5, 0x82, 0x37, 0x53, 0x28, 0x5c, 0x56, 0x65, 0x76, 0x95,
    0x59, 0xcd, 0x9f, 0x3a, 0x73, 0x89, 0xca, 0xac, 0x67, 0x9d, 0xf6, 0xd9, 0xf9, 0x91, 0x19, 0xdc,
    0x98, 0x6c, 0x46, 0x64, 0x6f, 0x26, 0x39, 0x62, 0xc6, 0x93, 0x09, 0x23, 0x06, 0x60, 0x8c, 0xc8,
    0x90, 0x32, 0x03, 0x3e, 0xc0, 0x13, 0x31, 0xe3, 0x4c, 0x9e, 0x0c, 0xc4, 0xcb, 0xbc, 0xb6, 0xe9,
    0x30, 0x6b, 0x1f, 0xf1, 0x1d, 0xd1, 0x47, 0x74, 0x4f, 0x10, 0x40, 0xf4, 0x01, 0x04, 0xa1, 0x1a,
    0xa4, 0x1b, 0x4a, 0xb1, 0xe4, 0x4e, 0x36, 0xc9, 0x9c, 0x63, 0x97, 0x71, 0x79, 0x17, 0x48, 0x69,
    0x21, 0x12, 0x3c, 0x84, 0x83, 0x6d, 0xd6, 0x96, 0xc3, 0x38, 0x1e, 0x4d, 0xe1, 0xd4, 0xc2, 0x2c,
    0x18, 0x81, 0xb4, 0x4b, 0x16, 0x61, 0xc1, 0x24, 0x1c, 0x49, 0x14, 0x42, 0x94, 0x41, 0x34, 0x43,
];

pub const BC4_PAIR: [[u8; 2]; 64] = [
    [1, 1], [0, 0], [3, 3], [6, 6], [4, 4], [5, 5], [3, 0], [0, 3],
    [6, 1], [1, 6], [7, 7], [2, 2], [6, 3], [3, 6], [1, 0], [0, 1],
    [4, 1], [1, 4], [3, 1], [1, 3], [6, 0], [0, 6], [4, 2], [7, 5],
    [5, 7], [2, 4], [0, 5], [5, 0], [6, 4], [4, 6], [5, 3], [3, 5],
    [4, 0], [0, 4], [1, 5], [5, 1], [3, 4], [4, 3], [2, 6], [6, 2],
    [7, 3], [3, 7], [1, 2], [2, 1], [0, 7], [7, 0], [2, 5], [5, 2],
    [7, 4], [4, 7], [4, 5], [5, 4], [3, 2], [2, 3], [0, 2], [2, 0],
    [1, 7], [7, 1], [6, 5], [5, 6], [6, 7], [7, 6], [2, 7], [7, 2],
];

pub const BC4_HISTORY_CONFIRMED: &[(usize, [u8; 2])] = &[
    (52, [217, 182]),
    (190, [43, 25]),
];

pub const BC5_PAIR: [[u8; 2]; 64] = [
    [0, 0], [1, 1], [4, 4], [3, 3], [6, 6], [7, 7], [4, 0], [0, 4],
    [0, 1], [2, 2], [1, 0], [3, 0], [0, 3], [5, 5], [2, 0], [0, 2],
    [1, 6], [6, 1], [4, 3], [1, 7], [3, 4], [1, 4], [7, 1], [6, 0],
    [0, 6], [6, 3], [6, 4], [3, 6], [3, 2], [4, 6], [1, 5], [1, 3],
    [2, 3], [5, 4], [5, 3], [5, 0], [4, 1], [7, 6], [4, 5], [4, 2],
    [3, 1], [3, 5], [5, 1], [6, 7], [2, 4], [7, 5], [7, 4], [0, 5],
    [6, 5], [4, 7], [5, 7], [7, 0], [5, 6], [0, 7], [7, 3], [3, 7],
    [5, 2], [2, 5], [1, 2], [6, 2], [2, 1], [2, 6], [7, 2], [2, 7],
];

pub const BC5_HISTORY_CONFIRMED: &[(usize, [u8; 4])] = &[
    (105, [219, 153, 195, 135]),
    (182, [133, 126, 124, 55]),
    (183, [128, 126, 116, 112]),
    (222, [131, 128, 123, 37]),
    (255, [129, 127, 128, 126]),
];

pub const BC3_HISTORY_CONFIRMED: &[(usize, [u8; 8])] = &[];

// Strict BC3, BC4 and BC5 BCPack block rebuilders, ported from Leonida's
// clean-room decoder (`tools/bcpack.py`, GPL-3.0).
//
// Only the entries Leonida marks *confirmed* are used. A chunk that consults
// an unconfirmed or unknown built-in history entry is refused with
// `E_DSTORAGE_DECOMPRESSION_ERROR` and a log line instead of guessing; that
// keeps the output byte-identical to the reference decoder in strict mode.

use crate::abi::{E_DSTORAGE_DECOMPRESSION_ERROR, E_DSTORAGE_INVALID_DESTINATION_SIZE};
use crate::bcpack::rans::{self, LaneOutput};

// `rans` models take `alphabet_bits`, which is the lane symbol width minus one
// (exactly as `BC1_ALPHABET_BITS` in the vendored `bc1.rs`).
pub const BC3_ALPHABET_BITS: [u8; 12] = [3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7];
pub const BC4_ALPHABET_BITS: [u8; 12] = [7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5];
pub const BC5_ALPHABET_BITS: [u8; 12] = [3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5];

const BC3_MODS: [i32; 8] = [256, 256, 32, 64, 32, 32, 64, 32];
const BC3_NONE: [u8; 8] = [128, 128, 0, 0, 0, 0, 0, 0];
const BC4_NONE: [u8; 2] = [128, 128];
const BC5_NONE: [u8; 4] = [128, 128, 128, 128];

fn report(what: &str, message: String) -> i32 {
    crate::record_raw("bcpack", what, &message);
    E_DSTORAGE_DECOMPRESSION_ERROR
}

fn invalid_destination(what: &str, message: String) -> i32 {
    crate::record_raw("bcpack", what, &message);
    E_DSTORAGE_INVALID_DESTINATION_SIZE
}

fn unzig(v: u32) -> i32 {
    if v & 1 != 0 {
        -(((v as i32) + 1) >> 1)
    } else {
        (v >> 1) as i32
    }
}

fn inter8(a: u16, b: u16) -> u32 {
    let mut d = 0u32;
    for k in 0..4 {
        d |= (((a >> k) & 1) as u32) << (2 * k);
        d |= (((b >> k) & 1) as u32) << (2 * k + 1);
    }
    d
}

fn history3(entry: isize) -> Option<[u8; 8]> {
    if entry < 0 {
        return None;
    }
    BC3_HISTORY_CONFIRMED
        .iter()
        .find(|(k, _)| *k == entry as usize)
        .map(|(_, v)| *v)
}

fn history4(entry: isize) -> Option<[u8; 2]> {
    if entry < 0 {
        return None;
    }
    BC4_HISTORY_CONFIRMED
        .iter()
        .find(|(k, _)| *k == entry as usize)
        .map(|(_, v)| *v)
}

fn history5(entry: isize) -> Option<[u8; 4]> {
    if entry < 0 {
        return None;
    }
    BC5_HISTORY_CONFIRMED
        .iter()
        .find(|(k, _)| *k == entry as usize)
        .map(|(_, v)| *v)
}

fn only_zero(lane: &[u16]) -> bool {
    lane.iter().all(|&s| s == 0)
}

fn bc3(lanes: &LaneOutput, nblocks: usize) -> Result<Vec<u8>, i32> {
    if !only_zero(lanes.lane(1)) {
        return Err(report("BC3", "lane 1 is non-zero (unexpected)".into()));
    }
    let l0 = lanes.lane(0);
    let alpha = lanes.lane(2);
    let rows = [
        lanes.lane(3),
        lanes.lane(4),
        lanes.lane(5),
        lanes.lane(6),
    ];
    let (lr, lg, lb) = (lanes.lane(7), lanes.lane(8), lanes.lane(9));
    let (top, bottom) = (lanes.lane(10), lanes.lane(11));
    let mut out = vec![0u8; nblocks * 16];
    let mut hist = vec![[0u8; 8]; nblocks];
    for i in 0..nblocks {
        let a = 2 * i;
        let b = 2 * i + 1;
        let d = (unzig(inter8(l0[a], l0[b])) & 255) as usize;
        let base: [u8; 8] = if d == 0 {
            BC3_NONE
        } else if i >= d {
            hist[i - d]
        } else {
            let entry = 256isize + i as isize - d as isize;
            match history3(entry) {
                Some(v) => v,
                None => {
                    return Err(report(
                        "BC3",
                        format!(
                            "block {i} references built-in history entry {entry}, which is not confirmed (strict mode)"
                        ),
                    ))
                }
            }
        };
        let dz = [alpha[a], alpha[b], lr[a], lg[a], lb[a], lr[b], lg[b], lb[b]];
        let mut e = [0u8; 8];
        for k in 0..8 {
            e[k] = (base[k] as i32 + unzig(dz[k] as u32)).rem_euclid(BC3_MODS[k]) as u8;
        }
        hist[i] = e;
        let mut bits = 0u64;
        for (r, row) in rows.iter().enumerate() {
            let left = BC3_ALPHA_PAIR[row[a] as usize];
            let right = BC3_ALPHA_PAIR[row[b] as usize];
            bits |= ((left[0] as u64)
                | ((left[1] as u64) << 3)
                | ((right[0] as u64) << 6)
                | ((right[1] as u64) << 9))
                << (12 * r);
        }
        let at = i * 16;
        out[at] = e[0];
        out[at + 1] = e[1];
        out[at + 2..at + 8].copy_from_slice(&bits.to_le_bytes()[..6]);
        let c0 = ((e[2] as u16) << 11) | ((e[3] as u16) << 5) | e[4] as u16;
        let c1 = ((e[5] as u16) << 11) | ((e[6] as u16) << 5) | e[7] as u16;
        out[at + 8..at + 10].copy_from_slice(&c0.to_le_bytes());
        out[at + 10..at + 12].copy_from_slice(&c1.to_le_bytes());
        let qa = BC3_COLOUR_QUADRANT[top[a] as usize];
        let qb = BC3_COLOUR_QUADRANT[top[b] as usize];
        let qc = BC3_COLOUR_QUADRANT[bottom[a] as usize];
        let qd = BC3_COLOUR_QUADRANT[bottom[b] as usize];
        out[at + 12] = (qa & 15) | ((qb & 15) << 4);
        out[at + 13] = (qa >> 4) | ((qb >> 4) << 4);
        out[at + 14] = (qc & 15) | ((qd & 15) << 4);
        out[at + 15] = (qc >> 4) | ((qd >> 4) << 4);
    }
    Ok(out)
}

fn bc4(lanes: &LaneOutput, nblocks: usize) -> Result<Vec<u8>, i32> {
    if !only_zero(lanes.lane(1)) {
        return Err(report("BC4", "lane 1 is non-zero (unexpected)".into()));
    }
    let l0 = lanes.lane(0);
    let (l2, l3) = (lanes.lane(2), lanes.lane(3));
    let rows = [
        lanes.lane(4),
        lanes.lane(5),
        lanes.lane(6),
        lanes.lane(7),
        lanes.lane(8),
        lanes.lane(9),
        lanes.lane(10),
        lanes.lane(11),
    ];
    let mut out = vec![0u8; nblocks * 8];
    let mut hist = vec![[0u8; 2]; nblocks];
    for i in 0..nblocks {
        let v = l0[i];
        let d = if v & 1 != 0 {
            256usize - (((v as usize) + 1) >> 1)
        } else {
            (v as usize) >> 1
        };
        let base: [u8; 2] = if d == 0 {
            BC4_NONE
        } else if i >= d {
            hist[i - d]
        } else {
            let entry = 256isize + i as isize - d as isize;
            match history4(entry) {
                Some(v) => v,
                None => {
                    return Err(report(
                        "BC4",
                        format!(
                            "block {i} references built-in history entry {entry}, which is not confirmed (strict mode)"
                        ),
                    ))
                }
            }
        };
        let a0 = ((base[0] as i32 + unzig(l2[i] as u32)) & 255) as u8;
        let a1 = ((base[1] as i32 + unzig(l3[i] as u32)) & 255) as u8;
        hist[i] = [a0, a1];
        let mut bits = 0u64;
        for r in 0..4 {
            let left = BC4_PAIR[rows[2 * r][i] as usize];
            let right = BC4_PAIR[rows[2 * r + 1][i] as usize];
            bits |= ((left[0] as u64)
                | ((left[1] as u64) << 3)
                | ((right[0] as u64) << 6)
                | ((right[1] as u64) << 9))
                << (12 * r);
        }
        let at = i * 8;
        out[at] = a0;
        out[at + 1] = a1;
        out[at + 2..at + 8].copy_from_slice(&bits.to_le_bytes()[..6]);
    }
    Ok(out)
}

fn bc5(lanes: &LaneOutput, nblocks: usize) -> Result<Vec<u8>, i32> {
    if !only_zero(lanes.lane(1)) {
        return Err(report("BC5", "lane 1 is non-zero (unexpected)".into()));
    }
    let l0 = lanes.lane(0);
    let (red, green) = (lanes.lane(2), lanes.lane(3));
    let rows = [
        lanes.lane(4),
        lanes.lane(5),
        lanes.lane(6),
        lanes.lane(7),
        lanes.lane(8),
        lanes.lane(9),
        lanes.lane(10),
        lanes.lane(11),
    ];
    let mut out = vec![0u8; nblocks * 16];
    let mut hist = vec![[0u8; 4]; nblocks];
    for i in 0..nblocks {
        let a = 2 * i;
        let b = 2 * i + 1;
        let d = (unzig(inter8(l0[a], l0[b])) & 255) as usize;
        let base: [u8; 4] = if d == 0 {
            BC5_NONE
        } else if i >= d {
            hist[i - d]
        } else {
            let entry = 256isize + i as isize - d as isize;
            match history5(entry) {
                Some(v) => v,
                None => {
                    return Err(report(
                        "BC5",
                        format!(
                            "block {i} references built-in history entry {entry}, which is not confirmed (strict mode)"
                        ),
                    ))
                }
            }
        };
        let e = [
            ((base[0] as i32 + unzig(red[a] as u32)) & 255) as u8,
            ((base[1] as i32 + unzig(red[b] as u32)) & 255) as u8,
            ((base[2] as i32 + unzig(green[a] as u32)) & 255) as u8,
            ((base[3] as i32 + unzig(green[b] as u32)) & 255) as u8,
        ];
        hist[i] = e;
        let at = i * 16;
        for ch in 0..2 {
            let mut bits = 0u64;
            for r in 0..4 {
                let left = BC5_PAIR[rows[4 * ch + r][a] as usize];
                let right = BC5_PAIR[rows[4 * ch + r][b] as usize];
                bits |= ((left[0] as u64)
                    | ((left[1] as u64) << 3)
                    | ((right[0] as u64) << 6)
                    | ((right[1] as u64) << 9))
                    << (12 * r);
            }
            let block = at + 8 * ch;
            out[block] = e[2 * ch];
            out[block + 1] = e[2 * ch + 1];
            out[block + 2..block + 8].copy_from_slice(&bits.to_le_bytes()[..6]);
        }
    }
    Ok(out)
}

/// Decode one BCPack chunk in mode 3 (BC3), 4 (BC4) or 5 (BC5) into whole
/// standard BCn blocks. Nothing is written on failure.
pub fn decode_strict(mode: u32, src: &[u8], dst: &mut [u8]) -> Result<(), i32> {
    let (alphabet, block_bytes, name) = match mode {
        3 => (&BC3_ALPHABET_BITS[..], 16usize, "BC3"),
        4 => (&BC4_ALPHABET_BITS[..], 8usize, "BC4"),
        5 => (&BC5_ALPHABET_BITS[..], 16usize, "BC5"),
        other => {
            return Err(report(
                "BCPack",
                format!("mode {other} has no supported strict rebuilder"),
            ))
        }
    };
    if dst.is_empty() || !dst.len().is_multiple_of(block_bytes) {
        return Err(invalid_destination(
            name,
            format!(
                "destination size {} is not a whole number of {block_bytes}-byte blocks",
                dst.len()
            ),
        ));
    }
    let nblocks = dst.len() / block_bytes;
    let symbols = dst.len() / 8; // ELEMENT_BYTES
    let lanes = match rans::decode_slice(src, alphabet, symbols) {
        Ok(l) => l,
        Err(e) => {
            return Err(report(
                name,
                format!("{symbols} elements of {} bytes do not decode: {e}", src.len()),
            ))
        }
    };
    let out = match mode {
        3 => bc3(&lanes, nblocks)?,
        4 => bc4(&lanes, nblocks)?,
        _ => bc5(&lanes, nblocks)?,
    };
    dst.copy_from_slice(&out);
    Ok(())
}
