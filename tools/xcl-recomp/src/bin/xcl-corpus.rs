//! `xcl-corpus <shaders.rpf | folder> [--compile] [--jobs N] [--limit N] [--json out.json]`
//!
//! Walks a console shader pack (or a folder of shader blobs) the way XCL's
//! `xcl shaders --precompile` does, translates every program with the vendored
//! recompiler and – with `--compile` – compiles the HLSL with the Linux `dxc` CLI
//! using XCL's exact arguments.
//!
//! The walk is the one of `shaders.rs` (`precompile`):
//!   * records of an RPF9 pack (`gpu::pack::scan` / `inflate`), or the files of a folder,
//!   * for every record: `gpu::xbox::parse`, then for every FSSM `gpu::recomp::interface`,
//!   * for every program of the FSSM: `gpu::recomp::translate` with
//!     `Options { mode, wave64: detect_wave64(code), ..Default::default() }`,
//!   * the DXC profiles of a mode: VS → vs_6_6, PS → ps_6_6, CS → cs_6_6,
//!     MS → cs_6_6 + ms_6_6 (the mesh pass-through), GS → skipped (it needs its
//!     pipeline partner).

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use xcl_recomp::gpu::{pack, recomp, xbox};
use xcl_recomp::{dxc, gpu};

/// The stage of a program (the mode the recompiler chose).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Stage {
    Vertex,
    Pixel,
    Compute,
    Mesh,
    Geometry,
}

impl std::fmt::Display for Stage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.name())
    }
}

impl Stage {
    fn name(self) -> &'static str {
        match self {
            Stage::Vertex => "VS",
            Stage::Pixel => "PS",
            Stage::Compute => "CS",
            Stage::Mesh => "MS",
            Stage::Geometry => "GS",
        }
    }

    fn label(self) -> &'static str {
        match self {
            Stage::Vertex => "vertex",
            Stage::Pixel => "pixel",
            Stage::Compute => "compute",
            Stage::Mesh => "mesh",
            Stage::Geometry => "geometry",
        }
    }
}

/// The counters of one stage.
#[derive(Debug, Default, Clone)]
struct StageStats {
    /// records (shader blobs) of this stage
    pub records: usize,
    /// programs of this stage
    pub programs: usize,
    /// programs the recompiler translated
    pub translated: usize,
    /// programs the recompiler translated without leaving an instruction out
    pub complete: usize,
    /// programs that failed to translate
    pub translate_errors: usize,
    /// HLSL compiled by DXC
    pub compiled: usize,
    /// HLSL that failed to compile
    pub compile_errors: usize,
    /// wave64 programs
    pub wave64: usize,
    /// wave32 programs
    pub wave32: usize,
    /// programs with a wave hazard (a cross-lane read that a 32-lane wave cannot do)
    pub wave_hazards: usize,
    /// programs refused for the host's wave size, by hazard kind (may overlap)
    pub refused_hazards: BTreeMap<String, usize>,
    /// the translate errors, by message
    pub translate_error_groups: BTreeMap<String, usize>,
    /// one example program name per translate error message
    pub translate_error_examples: BTreeMap<String, String>,
    /// the compile errors, by message
    pub compile_error_groups: BTreeMap<String, usize>,
    /// one example program name per compile error message
    pub compile_error_examples: BTreeMap<String, String>,
}

impl StageStats {
    fn merge(&mut self, other: &StageStats) {
        self.records += other.records;
        self.programs += other.programs;
        self.translated += other.translated;
        self.complete += other.complete;
        self.translate_errors += other.translate_errors;
        self.compiled += other.compiled;
        self.compile_errors += other.compile_errors;
        self.wave64 += other.wave64;
        self.wave32 += other.wave32;
        self.wave_hazards += other.wave_hazards;
        for (k, v) in &other.refused_hazards {
            *self.refused_hazards.entry(k.clone()).or_default() += v;
        }
        for (k, v) in &other.translate_error_groups {
            *self.translate_error_groups.entry(k.clone()).or_default() += v;
        }
        for (k, v) in &other.compile_error_groups {
            *self.compile_error_groups.entry(k.clone()).or_default() += v;
        }
        for (k, v) in &other.translate_error_examples {
            self.translate_error_examples.entry(k.clone()).or_insert_with(|| v.clone());
        }
        for (k, v) in &other.compile_error_examples {
            self.compile_error_examples.entry(k.clone()).or_insert_with(|| v.clone());
        }
    }
}

/// The counters of the whole run.
#[derive(Debug, Default, Clone)]
struct Totals {
    /// records of the pack (or files of the folder)
    pub records: usize,
    /// records that could not be unpacked
    pub inflate_errors: usize,
    /// records that are not console containers
    pub not_console: usize,
    /// programs of all stages
    pub programs: usize,
    /// programs the recompiler translated
    pub translated: usize,
    /// programs that failed to translate
    pub translate_errors: usize,
    /// programs of a mode the corpus does not compile (GS)
    pub skipped: usize,
    /// ES+GS pairs assembled and translated through XCL's two-pass path
    pub gs_pairs: usize,
    /// ES+GS pairs XCL could not assemble / translate
    pub gs_pair_errors: usize,
    /// DXIL jobs compiled from the two-pass geometry programs
    pub gs_pair_compiled: usize,
    /// DXIL jobs of the two-pass geometry programs that failed
    pub gs_pair_compile_errors: usize,
    /// wave64 programs
    pub wave64: usize,
    /// wave32 programs
    pub wave32: usize,
    /// programs with a wave hazard
    pub wave_hazards: usize,
    /// programs refused for the host's wave size, by hazard kind (may overlap)
    pub refused_hazards: BTreeMap<String, usize>,
    /// HLSL compiled by DXC
    pub compiled: usize,
    /// HLSL that failed to compile
    pub compile_errors: usize,
    /// programs whose translation was not complete (some instruction left out)
    pub incomplete: usize,
    /// the stages
    pub stages: BTreeMap<Stage, StageStats>,
}

impl Totals {
    fn merge(&mut self, other: &Totals) {
        self.records += other.records;
        self.inflate_errors += other.inflate_errors;
        self.not_console += other.not_console;
        self.programs += other.programs;
        self.translated += other.translated;
        self.translate_errors += other.translate_errors;
        self.skipped += other.skipped;
        self.gs_pairs += other.gs_pairs;
        self.gs_pair_errors += other.gs_pair_errors;
        self.gs_pair_compiled += other.gs_pair_compiled;
        self.gs_pair_compile_errors += other.gs_pair_compile_errors;
        self.wave64 += other.wave64;
        self.wave32 += other.wave32;
        self.wave_hazards += other.wave_hazards;
        for (k, v) in &other.refused_hazards {
            *self.refused_hazards.entry(k.clone()).or_default() += v;
        }
        self.compiled += other.compiled;
        self.compile_errors += other.compile_errors;
        self.incomplete += other.incomplete;
        for (stage, stats) in &other.stages {
            self.stages.entry(*stage).or_default().merge(stats);
        }
    }
}

/// What the corpus tool reads: an RPF9 pack or a folder of blobs.
enum Input {
    Pack { data: Vec<u8>, records: Vec<pack::Record> },
    Files(Vec<PathBuf>),
}

impl Input {
    fn len(&self) -> usize {
        match self {
            Input::Pack { records, .. } => records.len(),
            Input::Files(f) => f.len(),
        }
    }

    /// (name, blob); the blob is None when the record does not inflate.
    fn get(&self, i: usize) -> (String, Option<Vec<u8>>) {
        match self {
            Input::Pack { data, records } => (format!("{} @{:#x}", records[i].name, records[i].offset), pack::inflate(data, &records[i])),
            Input::Files(f) => (f[i].display().to_string(), std::fs::read(&f[i]).ok()),
        }
    }
}

fn collect_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    let mut entries: Vec<PathBuf> = rd.filter_map(|e| e.ok().map(|e| e.path())).collect();
    entries.sort();
    for p in entries {
        if p.is_dir() {
            collect_files(&p, out);
            continue;
        }
        let name = p.file_name().map(|n| n.to_string_lossy().to_lowercase()).unwrap_or_default();
        if name.ends_with(".txt") || name.contains(".fssm") {
            continue;
        }
        out.push(p);
    }
}

/// The error line of a DXC message, without the source excerpt.
///
/// DXC prints "file.hlsl:12:5: error: message" (and the offending source line and a
/// caret). Only the message is kept, so the failures group by their reason.
fn dxc_error_line(message: &str) -> String {
    // the message of `dxc::compile` starts with "dxc: "
    let text = message.strip_prefix("dxc: ").unwrap_or(message);
    for line in text.lines() {
        let line = line.trim();
        // "file.hlsl:12:5: error: message" (the position may be missing)
        if let Some(i) = line.find(": error: ") {
            return line[i + ": error: ".len()..].trim().to_string();
        }
        if let Some(i) = line.find("error: ") {
            return line[i + "error: ".len()..].trim().to_string();
        }
    }
    // no "error:" line: keep the first non-empty line (e.g. "dxc: cannot run dxc")
    text.lines().map(|l| l.trim()).find(|l| !l.is_empty()).unwrap_or("").to_string()
}

/// The stage of a recompiler mode.
fn stage_of(mode: &recomp::Mode) -> Stage {
    match mode {
        recomp::Mode::Vertex(_) => Stage::Vertex,
        recomp::Mode::Pixel(_) => Stage::Pixel,
        recomp::Mode::Compute(_) => Stage::Compute,
        recomp::Mode::Mesh(_) => Stage::Mesh,
        recomp::Mode::Geometry(_) => Stage::Geometry,
        recomp::Mode::Kernel => Stage::Compute,
    }
}

/// The DXC jobs of a mode: (profile, source). `None` means the translated HLSL.
fn jobs_of(mode: &recomp::Mode) -> Vec<(String, Option<String>)> {
    match mode {
        recomp::Mode::Vertex(_) => vec![("vs_6_6".to_string(), None)],
        recomp::Mode::Pixel(_) => vec![("ps_6_6".to_string(), None)],
        recomp::Mode::Compute(_) => vec![("cs_6_6".to_string(), None)],
        recomp::Mode::Mesh(ms) => vec![("cs_6_6".to_string(), None), ("ms_6_6".to_string(), Some(recomp::mesh_passthrough(ms)))],
        recomp::Mode::Geometry(_) => vec![],
        recomp::Mode::Kernel => vec![("cs_6_6".to_string(), None)],
    }
}

/// Translate and compile one record.
fn process(name: &str, blob: &[u8], compile: bool, host_wave: u32, totals: &mut Totals) {
    totals.records += 1;
    let Some(sh) = xbox::parse(blob) else {
        totals.not_console += 1;
        return;
    };
    let mut record_stages = Vec::new();
    for f in &sh.fssm {
        let Some(mode) = recomp::interface(blob, &sh, f) else { continue };
        let stage = stage_of(&mode);
        if !record_stages.contains(&stage) {
            totals.stages.entry(stage).or_default().records += 1;
            record_stages.push(stage);
        }
        let jobs = jobs_of(&mode);
        for p in &f.programs {
            totals.programs += 1;
            let stats = totals.stages.entry(stage).or_default();
            stats.programs += 1;

            let code = &f.data[p.offset..p.offset + p.len];
            let wave64 = recomp::detect_wave64(code);
            if wave64 {
                stats.wave64 += 1;
                totals.wave64 += 1;
            } else {
                stats.wave32 += 1;
                totals.wave32 += 1;
            }
            let hazards = if host_wave == 32 { recomp::host_hazards(code, &mode, host_wave) } else { recomp::wave_hazards(code) };
            if !hazards.is_empty() {
                stats.wave_hazards += 1;
                totals.wave_hazards += 1;
            }

            // A 32-lane host runs a wave64 program in two native wave32 halves.
            let opt = recomp::Options {
                mode: mode.clone(),
                wave64,
                hw_wave: host_wave,
                geometry_wave32: host_wave == 32 && matches!(mode, recomp::Mode::Geometry(_)),
                mesh_wave32: host_wave == 32 && matches!(mode, recomp::Mode::Mesh(_)),
                host_wave,
                ..Default::default()
            };
            let translation = match recomp::translate(code, &opt) {
                Ok(t) => t,
                Err(e) => {
                    stats.translate_errors += 1;
                    totals.translate_errors += 1;
                    for hazard in &hazards {
                        *stats.refused_hazards.entry((*hazard).to_string()).or_default() += 1;
                        *totals.refused_hazards.entry((*hazard).to_string()).or_default() += 1;
                    }
                    let message = e.lines().next().unwrap_or(&e).trim().to_string();
                    *stats.translate_error_groups.entry(message.clone()).or_default() += 1;
                    stats.translate_error_examples.entry(message).or_insert_with(|| name.to_string());
                    continue;
                }
            };
            stats.translated += 1;
            totals.translated += 1;
            if !translation.complete() {
                stats.complete += 0;
                totals.incomplete += 1;
            } else {
                stats.complete += 1;
            }

            if jobs.is_empty() {
                // GS: it needs its pipeline partner, the runtime pairs it.
                totals.skipped += 1;
                continue;
            }
            if !compile {
                continue;
            }
            for (profile, source) in &jobs {
                let hlsl = source.as_deref().unwrap_or(&translation.hlsl);
                match dxc::compile_cached(hlsl, "main", profile) {
                    Ok(_) => {
                        stats.compiled += 1;
                        totals.compiled += 1;
                    }
                    Err(e) => {
                        stats.compile_errors += 1;
                        totals.compile_errors += 1;
                        let message = dxc_error_line(&e);
                        *stats.compile_error_groups.entry(message.clone()).or_default() += 1;
                        stats.compile_error_examples.entry(message).or_insert_with(|| name.to_string());
                    }
                }
            }
        }
    }
}

/// The ES variant of a VS and a GS record share the name's hash; the pack holds one
/// of each per geometry program. Assemble them the way XCL's `console_geometry` does
/// and compile the compute + mesh pass-through pair.
fn process_gs_pairs(input: &Input, compile: bool, host_wave: u32, totals: &mut Totals) {
    let key = |name: &str| -> String {
        // Input::get includes the record offset: "NAME @0x...".
        let name = name.split(" @").next().unwrap_or(name);
        name.split_once('_').map(|(_, tail)| tail).unwrap_or(name).trim_end_matches("_Wrapped").to_string()
    };
    let mut es: BTreeMap<String, Vec<Vec<u8>>> = BTreeMap::new();
    let mut gs: BTreeMap<String, Vec<Vec<u8>>> = BTreeMap::new();
    for i in 0..input.len() {
        let (name, blob) = input.get(i);
        let Some(blob) = blob else { continue };
        let Some(sh) = xbox::parse(&blob) else { continue };
        let hw = sh.fssm.first().and_then(|f| f.stages.first()).map(|s| s.hw);
        match (sh.stage.as_str(), hw) {
            ("VS", Some(2)) => es.entry(key(&name)).or_default().push(blob),
            ("GS", Some(3)) => gs.entry(key(&name)).or_default().push(blob),
            _ => {}
        }
    }
    for (name, programs) in gs {
        let Some(es_programs) = es.remove(&name) else {
            eprintln!("xcl-corpus: GS {name}: no ES variant in the pack");
            totals.gs_pair_errors += programs.len();
            continue;
        };
        if es_programs.len() != programs.len() {
            eprintln!("xcl-corpus: GS {name}: {} programs but {} ES variants", programs.len(), es_programs.len());
            totals.gs_pair_errors += programs.len().max(es_programs.len());
            continue;
        }
        for (es_blob, gs_blob) in es_programs.into_iter().zip(programs) {
            let (code, interface) = match recomp::geometry_program(&es_blob, &gs_blob) {
                Ok(p) => p,
                Err(e) => {
                    eprintln!("xcl-corpus: GS {name}: {e}");
                    totals.gs_pair_errors += 1;
                    continue;
                }
            };
            let translation = match recomp::translate_geometry(&code, interface.clone(), host_wave == 32) {
                Ok(t) => t,
                Err(e) => {
                    eprintln!("xcl-corpus: GS {name}: {e}");
                    totals.gs_pair_errors += 1;
                    continue;
                }
            };
            totals.gs_pairs += 1;
            if !compile {
                continue;
            }
            let passthrough = recomp::mesh_passthrough(&interface.record());
            for (profile, source) in [("cs_6_6", translation.hlsl.as_str()), ("ms_6_6", passthrough.as_str())] {
                match dxc::compile_cached(source, "main", profile) {
                    Ok(_) => totals.gs_pair_compiled += 1,
                    Err(e) => {
                        eprintln!("xcl-corpus: GS {name} {profile}: {}", dxc_error_line(&e));
                        totals.gs_pair_compile_errors += 1;
                    }
                }
            }
        }
    }
}

fn usage() -> ! {
    eprintln!(
        "xcl-corpus <shaders.rpf | folder> [--compile] [--jobs N] [--limit N] [--host-wave 32|64] [--json out.json]\n\
         \n\
         Translates every program of a console shader pack with XCL's recompiler.\n\
         With --compile the HLSL is compiled with the Linux dxc CLI (XCL's arguments).\n\
         --host-wave 32 models a GPU with 32-lane subgroups: wave64 mesh/geometry programs\n\
         run as two halves and wave64 programs with cross-half hazards are refused.\n\
         \n\
         Environment:\n\
         \x20 XCL_DXC        the dxc executable (default: dxc on the PATH)\n\
         \x20 XCL_DXIL_CACHE the DXIL cache (default: ~/.cache/xvalker/dxil-cache)"
    );
    std::process::exit(2);
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut path: Option<PathBuf> = None;
    let mut compile = false;
    let mut jobs = 24usize;
    let mut limit = usize::MAX;
    let mut host_wave = 0u32;
    let mut json: Option<PathBuf> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--compile" => compile = true,
            "--jobs" => {
                i += 1;
                jobs = args.get(i).and_then(|v| v.parse().ok()).unwrap_or_else(|| usage());
            }
            "--limit" => {
                i += 1;
                limit = args.get(i).and_then(|v| v.parse().ok()).unwrap_or_else(|| usage());
            }
            "--host-wave" => {
                i += 1;
                host_wave = args.get(i).and_then(|v| v.parse().ok()).unwrap_or_else(|| usage());
                if host_wave != 32 && host_wave != 64 {
                    usage();
                }
            }
            "--json" => {
                i += 1;
                json = args.get(i).map(PathBuf::from);
                if json.is_none() {
                    usage();
                }
            }
            "--help" | "-h" => usage(),
            other if other.starts_with("--") => {
                eprintln!("xcl-corpus: unknown option {other}");
                usage();
            }
            other => {
                if path.is_some() {
                    eprintln!("xcl-corpus: only one input path");
                    usage();
                }
                path = Some(PathBuf::from(other));
            }
        }
        i += 1;
    }
    let Some(path) = path else { usage() };

    let t0 = Instant::now();
    let input = if path.is_dir() {
        let mut files = Vec::new();
        collect_files(&path, &mut files);
        let n = files.len();
        eprintln!("xcl-corpus: folder, {n} files");
        Input::Files(files)
    } else {
        let data = match std::fs::read(&path) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("xcl-corpus: {}: {e}", path.display());
                std::process::exit(2);
            }
        };
        if data.starts_with(pack::RPF9_MAGIC) {
            let records = pack::scan(&data, 0x40);
            eprintln!("xcl-corpus: RPF9 package {:.1} MB, {} shader records", data.len() as f64 / 1048576.0, records.len());
            Input::Pack { data, records }
        } else {
            eprintln!("xcl-corpus: {} is not an RPF9 package", path.display());
            std::process::exit(2);
        }
    };

    let n = input.len().min(limit);
    if compile {
        // The compiler must be there before the run: a missing dxc would otherwise
        // show up as thousands of identical compile errors.
        match std::process::Command::new(dxc::dxc_path()).arg("--version").output() {
            Ok(out) if out.status.success() => {
                let version = String::from_utf8_lossy(&out.stdout).trim().to_string();
                eprintln!("xcl-corpus: dxc {} ({})", dxc::dxc_path().display(), version.lines().next().unwrap_or(""));
            }
            Ok(out) => {
                eprintln!("xcl-corpus: {} does not run: {}", dxc::dxc_path().display(), String::from_utf8_lossy(&out.stderr).trim());
                std::process::exit(2);
            }
            Err(e) => {
                eprintln!("xcl-corpus: cannot run {}: {e} (run tools/fetch-dxc-linux.sh)", dxc::dxc_path().display());
                std::process::exit(2);
            }
        }
    }
    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let parts: Vec<Totals> = std::thread::scope(|s| {
        let handles: Vec<_> = (0..jobs.max(1))
            .map(|_| {
                s.spawn(|| {
                    let mut totals = Totals::default();
                    loop {
                        let i = next.fetch_add(1, Ordering::Relaxed);
                        if i >= n {
                            break;
                        }
                        let (name, blob) = input.get(i);
                        let Some(blob) = blob else {
                            totals.inflate_errors += 1;
                            continue;
                        };
                        process(&name, &blob, compile, host_wave, &mut totals);
                        let d = done.fetch_add(1, Ordering::Relaxed) + 1;
                        if d % 1000 == 0 {
                            eprintln!("  {d}/{n} ({:.0} s)", t0.elapsed().as_secs_f64());
                        }
                    }
                    totals
                })
            })
            .collect();
        handles.into_iter().map(|h| h.join().unwrap()).collect()
    });
    let mut totals = Totals::default();
    for part in &parts {
        totals.merge(part);
    }
    if compile {
        process_gs_pairs(&input, compile, host_wave, &mut totals);
    }

    report(&path, &totals, compile, limit, host_wave, t0.elapsed().as_secs_f64());

    if let Some(json) = json {
        let value = to_json(&totals, compile, limit, jobs, host_wave, t0.elapsed().as_secs_f64());
        let text = serde_json::to_string_pretty(&value).unwrap_or_else(|e| format!("{{\"error\": \"{e}\"}}"));
        if let Err(e) = std::fs::write(&json, text) {
            eprintln!("xcl-corpus: cannot write {}: {e}", json.display());
            std::process::exit(2);
        }
        eprintln!("xcl-corpus: report written to {}", json.display());
    }
}

/// The top `n` groups of a map, most frequent first.
fn top_groups(groups: &BTreeMap<String, usize>, examples: &BTreeMap<String, String>, n: usize) -> Vec<(String, usize, String)> {
    let mut v: Vec<_> = groups.iter().map(|(k, c)| (k.clone(), *c, examples.get(k).cloned().unwrap_or_default())).collect();
    v.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    v.truncate(n);
    v
}

fn report(path: &Path, totals: &Totals, compile: bool, limit: usize, host_wave: u32, seconds: f64) {
    println!("{}", path.display());
    println!("host wave {}, wave hazards {}, refused for host {}", if host_wave == 0 { "native".to_string() } else { host_wave.to_string() }, totals.wave_hazards, totals.translate_errors);
    println!(
        "records {}, programs {}, translated {} ({} complete), translate errors {}",
        totals.records, totals.programs, totals.translated, totals.translated - totals.incomplete, totals.translate_errors
    );
    println!(
        "wave64 programs {}, wave32 programs {}, wave hazards {} programs, skipped (GS) {}",
        totals.wave64, totals.wave32, totals.wave_hazards, totals.skipped
    );
    if compile {
        println!("compiled {}, compile errors {}", totals.compiled, totals.compile_errors);
        println!(
            "GS two-pass: {} ES+GS pairs, {} errors, {} DXIL compiled, {} compile errors",
            totals.gs_pairs, totals.gs_pair_errors, totals.gs_pair_compiled, totals.gs_pair_compile_errors
        );
    } else {
        println!("not compiled (no --compile)");
    }
    println!("per stage:");
    for (stage, stats) in &totals.stages {
        println!(
            "  {:<8} records {}, programs {}, translated {}, translate errors {}, compiled {}, compile errors {}, wave64 {}, wave hazards {}",
            stage.name(),
            stats.records,
            stats.programs,
            stats.translated,
            stats.translate_errors,
            stats.compiled,
            stats.compile_errors,
            stats.wave64,
            stats.wave_hazards
        );
        for (hazard, count) in &stats.refused_hazards {
            println!("    [{stage}] refused {count:>6}  {hazard}");
        }
    }
    println!("top translate errors:");
    for (stage, stats) in &totals.stages {
        for (message, count, example) in top_groups(&stats.translate_error_groups, &stats.translate_error_examples, 30) {
            println!("  [{stage}] {count:>6}  {message}  (e.g. {example})");
        }
    }
    if compile {
        println!("top compile errors:");
        for (stage, stats) in &totals.stages {
            for (message, count, example) in top_groups(&stats.compile_error_groups, &stats.compile_error_examples, 30) {
                println!("  [{stage}] {count:>6}  {message}  (e.g. {example})");
            }
        }
    }
    println!("limit {}, time {:.1} s", if limit == usize::MAX { "none".to_string() } else { limit.to_string() }, seconds);
}

fn to_json(totals: &Totals, compile: bool, limit: usize, jobs: usize, host_wave: u32, seconds: f64) -> serde_json::Value {
    let mut stages = serde_json::Map::new();
    for (stage, stats) in &totals.stages {
        let translate_errors: Vec<serde_json::Value> = top_groups(&stats.translate_error_groups, &stats.translate_error_examples, 30)
            .into_iter()
            .map(|(message, count, example)| serde_json::json!({ "message": message, "count": count, "example": example }))
            .collect();
        let compile_errors: Vec<serde_json::Value> = top_groups(&stats.compile_error_groups, &stats.compile_error_examples, 30)
            .into_iter()
            .map(|(message, count, example)| serde_json::json!({ "message": message, "count": count, "example": example }))
            .collect();
        stages.insert(
            stage.name().to_string(),
            serde_json::json!({
                "label": stage.label(),
                "records": stats.records,
                "programs": stats.programs,
                "translated": stats.translated,
                "complete": stats.complete,
                "translate_errors": stats.translate_errors,
                "translate_error_groups": translate_errors,
                "compiled": stats.compiled,
                "compile_errors": stats.compile_errors,
                "compile_error_groups": compile_errors,
                "wave64": stats.wave64,
                "wave32": stats.wave32,
                "wave_hazards": stats.wave_hazards,
                "refused": stats.translate_errors,
                "refused_hazards": stats.refused_hazards,
            }),
        );
    }
    serde_json::json!({
        "records": totals.records,
        "inflate_errors": totals.inflate_errors,
        "not_console_containers": totals.not_console,
        "programs": totals.programs,
        "translated": totals.translated,
        "complete": totals.translated - totals.incomplete,
        "incomplete": totals.incomplete,
        "translate_errors": totals.translate_errors,
        "skipped": totals.skipped,
        "gs_pairs": totals.gs_pairs,
        "gs_pair_errors": totals.gs_pair_errors,
        "gs_pair_compiled": totals.gs_pair_compiled,
        "gs_pair_compile_errors": totals.gs_pair_compile_errors,
        "wave64": totals.wave64,
        "wave32": totals.wave32,
        "wave_hazards": totals.wave_hazards,
        "refused_hazards": totals.refused_hazards,
        "host_wave": host_wave,
        "compiled": totals.compiled,
        "compile_errors": totals.compile_errors,
        "compile_requested": compile,
        "limit": if limit == usize::MAX { serde_json::Value::Null } else { serde_json::Value::from(limit) },
        "jobs": jobs,
        "stages": serde_json::Value::Object(stages),
        "seconds": seconds,
    })
}

// The crate is linked as a library too; keep the reference so the link step does not
// drop the recompiler when only the binary is built.
#[allow(dead_code)]
fn _unused(_: &gpu::recomp::Options) {}
