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
    /// wave64 programs
    pub wave64: usize,
    /// wave32 programs
    pub wave32: usize,
    /// programs with a wave hazard
    pub wave_hazards: usize,
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
        self.wave64 += other.wave64;
        self.wave32 += other.wave32;
        self.wave_hazards += other.wave_hazards;
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
fn process(name: &str, blob: &[u8], compile: bool, totals: &mut Totals) {
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
            let hazards = recomp::wave_hazards(code);
            if !hazards.is_empty() {
                stats.wave_hazards += 1;
                totals.wave_hazards += 1;
            }

            let opt = recomp::Options { mode: mode.clone(), wave64, ..Default::default() };
            let translation = match recomp::translate(code, &opt) {
                Ok(t) => t,
                Err(e) => {
                    stats.translate_errors += 1;
                    totals.translate_errors += 1;
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

fn usage() -> ! {
    eprintln!(
        "xcl-corpus <shaders.rpf | folder> [--compile] [--jobs N] [--limit N] [--json out.json]\n\
         \n\
         Translates every program of a console shader pack with XCL's recompiler.\n\
         With --compile the HLSL is compiled with the Linux dxc CLI (XCL's arguments).\n\
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
                        process(&name, &blob, compile, &mut totals);
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

    report(&path, &totals, compile, limit, t0.elapsed().as_secs_f64());

    if let Some(json) = json {
        let value = to_json(&totals, compile, limit, jobs, t0.elapsed().as_secs_f64());
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

fn report(path: &Path, totals: &Totals, compile: bool, limit: usize, seconds: f64) {
    println!("{}", path.display());
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

fn to_json(totals: &Totals, compile: bool, limit: usize, jobs: usize, seconds: f64) -> serde_json::Value {
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
        "wave64": totals.wave64,
        "wave32": totals.wave32,
        "wave_hazards": totals.wave_hazards,
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
