//! Decodes one BCPack chunk and writes the blocks.
//!
//!     cargo run --example decode -- <mode> <chunk> <size> [out]

use std::io::Write;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 4 {
        eprintln!("usage: decode <mode> <chunk> <size> [out]");
        std::process::exit(2);
    }
    let mode: u32 = args[1].parse().expect("mode");
    let chunk = std::fs::read(&args[2]).expect("chunk");
    let size: usize = args[3].parse().expect("size");
    let mut out = vec![0u8; size];
    // SAFETY: the chunk and the destination are readable and writable for the
    // sizes given.
    let hr = unsafe {
        bcpack::xv_bcpack_decode(
            mode,
            chunk.as_ptr(),
            chunk.len(),
            out.as_mut_ptr(),
            out.len(),
        )
    };
    if hr != 0 {
        eprintln!("decode failed: 0x{hr:08x}");
        std::process::exit(1);
    }
    match args.get(4) {
        Some(path) => std::fs::File::create(path)
            .and_then(|mut f| f.write_all(&out))
            .expect("write"),
        None => {
            let mut stdout = std::io::stdout();
            stdout.write_all(&out).expect("write");
        }
    }
}
