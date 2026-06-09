//! `unpin-man` — the `unpin man` helper verb, in Rust.
//!
//! Given `man [pkg] [page]` (a missing pkg defaults to `unpin`, so a bare
//! `unpin man` shows unpin's own manual), it enumerates the package's embedded
//! `unpin/man/` pages via `unpin bundle list`, picks the right (name, section,
//! lang), follows whole-page `.so` redirects, dumps that entry's roff with
//! `unpin bundle dump`, then renders and pages it. Building it as a plain Rust
//! crate (no cosmo) is what lets it ship a native Windows `.exe` via mingw, the
//! way `unpin` and `unpin-readme` do. It supersedes an earlier C front-end that
//! drove mandoc's `main` over a subprocess; the renderer is linked in-process
//! now (see [`mandoc`]).
//!
//! The split: bundle resolution lives here (ported from the C front-end), the
//! pager in [`render`] (copied from unpin-readme for now), and roff→ANSI
//! rendering in [`mandoc`] (the vendored mandoc render-only subset, in-process).
//! unpin knows nothing about man; this is the other half.
//!
//! `$UNPIN_SELF` is the absolute path of the unpin binary that launched us (set
//! by `unpin run`); we fall back to `unpin` on `PATH` when it is unset, so the
//! package is still usable when invoked by hand.

use std::ffi::OsStr;
use std::process::{Command, ExitCode};

mod mandoc;
mod render;

/// `--help` text. Leads with the fact that this is the `unpin man` verb —
/// normally reached *through* unpin, not run on its own.
const HELP: &str = "\
unpin-man — render an unpins program's manual pages in your terminal

This is the helper behind the `unpin man` verb. You normally reach it
through unpin, which fetches and runs it on demand and never puts it on PATH:

    unpin man <pkg>              show unpins/<pkg>'s manual
    unpin man <pkg> <page>       show a specific page
    unpin man                    show unpin's own manual

Run directly it behaves the same:

    unpin-man <pkg> [page]
    unpin-man -                  read roff from stdin

Options:
    -h, --help     print this help and exit
    -V, --version  print version and exit

Pager keys: q quit · ↑/↓ j/k scroll · Space/b page · g/G top/bottom
";

/// One embedded page enumerated from `unpin bundle list`.
#[derive(Clone)]
struct Entry {
    name: String,
    lang: String,
    section: u32,
    /// Full bundle path, e.g. `unpin/man/ls.1`.
    path: String,
    /// `Some((target_name, target_section))` for a `.so` whole-page redirect.
    redirect: Option<(String, u32)>,
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();

    // Flags win over a package name (no real package is called `--help`).
    match args.first().map(String::as_str) {
        Some("-h") | Some("--help") => {
            print!("{HELP}");
            return ExitCode::SUCCESS;
        }
        Some("-V") | Some("--version") => {
            println!("{} {}", env!("CARGO_PKG_NAME"), env!("CARGO_PKG_VERSION"));
            return ExitCode::SUCCESS;
        }
        _ => {}
    }

    // No package given (`unpin man`) defaults to unpin's own manual; the default
    // page is the package name (`man ls` ⇒ page `ls`).
    let pkg = args.first().cloned().unwrap_or_else(|| "unpin".to_owned());
    let page = args.get(1).cloned().unwrap_or_else(|| pkg.clone());

    // `-`: read roff straight from stdin and page it (dev/testing — no bundle).
    if pkg == "-" {
        let mut roff = String::new();
        if std::io::Read::read_to_string(&mut std::io::stdin(), &mut roff).is_err() {
            eprintln!("man: reading stdin");
            return ExitCode::FAILURE;
        }
        render::page(&roff);
        return ExitCode::SUCCESS;
    }

    match run(&pkg, &page) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("man: {e}");
            ExitCode::FAILURE
        }
    }
}

/// Resolve and page `page` from `pkg`'s embedded manual.
fn run(pkg: &str, page: &str) -> Result<(), String> {
    let unpin = std::env::var_os("UNPIN_SELF")
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "unpin".into());

    // 1. enumerate the package's embedded pages.
    let listing = capture(&unpin, &["bundle", "list", pkg])?;
    let ents: Vec<Entry> = listing.lines().filter_map(parse_line).collect();
    if ents.is_empty() {
        return Err(format!("`{pkg}` has no embedded manual"));
    }

    // 2. pick + follow `.so` → the roff entry to render.
    let entry_path = resolve(&ents, page, "en")?;

    // 3. dump that entry's roff and page it.
    let roff = capture(&unpin, &["bundle", "dump", pkg, &entry_path])?;
    render::page(&roff);
    Ok(())
}

/// Run `unpin <args...>` and capture stdout. A non-zero exit is a real failure
/// (pkg not installed, unreadable binary, corrupt bundle) — matching the bundle
/// family's "absence is not an error, but a broken read is" contract.
fn capture(unpin: &OsStr, args: &[&str]) -> Result<String, String> {
    let out = Command::new(unpin)
        .args(args)
        .output()
        .map_err(|e| format!("running `unpin {}`: {e}", args.join(" ")))?;
    if !out.status.success() {
        return Err(format!("`unpin {}` failed", args.join(" ")));
    }
    String::from_utf8(out.stdout).map_err(|_| "bundle output is not valid UTF-8".to_owned())
}

/// Leading decimal digits of a section token: `"1"` → 1, `"3pm"` → 3, `"x"` → 0.
fn parse_section(s: &str) -> u32 {
    s.bytes()
        .take_while(u8::is_ascii_digit)
        .fold(0, |n, b| n * 10 + u32::from(b - b'0'))
}

/// Parse one `bundle list` line into an [`Entry`], or `None` for non-man lines
/// (aliases, future kinds, blanks). Line shapes (see unpin `src/bundle.rs`):
///
/// ```text
/// unpin/man/ls.1\t909            regular roff page (size after the tab)
/// unpin/man/dir.1\t-> ls.1       `.so` redirect (target after `-> `)
/// unpin/man/pt_BR/ls.1\t909      a non-default language page
/// ```
fn parse_line(line: &str) -> Option<Entry> {
    let (left, rhs) = line.split_once('\t')?;
    let rel = left.strip_prefix("unpin/man/")?;

    // `<lang>/<file>` or, with no slash, the default language `en`.
    let (lang, file) = match rel.split_once('/') {
        Some((lang, file)) => {
            if file.contains('/') {
                return None; // nested beyond one lang dir — skip
            }
            (lang.to_owned(), file)
        }
        None => ("en".to_owned(), rel),
    };

    let (name, sect) = file.rsplit_once('.')?; // no section suffix — not a page
    let redirect = rhs.strip_prefix("-> ").map(|tgt| {
        let tgt = tgt.trim_start();
        let base = tgt.rsplit('/').next().unwrap_or(tgt);
        let (tname, tsect) = base.rsplit_once('.').unwrap_or((base, ""));
        (tname.to_owned(), parse_section(tsect))
    });

    Some(Entry {
        name: name.to_owned(),
        lang,
        section: parse_section(sect),
        path: left.to_owned(),
        redirect,
    })
}

/// Pick the best page for (name, section, lang): prefer `lang`, fall back to
/// `en`; with `section == None` take the lowest-numbered one. Mirrors unpin
/// `src/man.rs::pick`.
fn pick<'a>(
    ents: &'a [Entry],
    name: &str,
    section: Option<u32>,
    lang: &str,
) -> Option<&'a Entry> {
    for l in [lang, "en"] {
        let mut best: Option<&Entry> = None;
        for e in ents {
            if e.name != name || e.lang != l {
                continue;
            }
            if section.is_some_and(|s| e.section != s) {
                continue;
            }
            if best.is_none_or(|b| e.section < b.section) {
                best = Some(e);
            }
        }
        if best.is_some() {
            return best;
        }
        if lang == "en" {
            break; // lang == "en": don't scan twice
        }
    }
    None
}

/// Resolve (name, lang) to the bundle path of a roff page, following whole-page
/// `.so` redirects up to `MAX_SO_DEPTH` with cycle detection. Mirrors unpin
/// `src/man.rs::roff_for`.
fn resolve(ents: &[Entry], name: &str, lang: &str) -> Result<String, String> {
    const MAX_SO_DEPTH: usize = 4;

    let mut cur_name = name.to_owned();
    let mut cur_section: Option<u32> = None; // unspecified
    let mut seen: Vec<(String, Option<u32>)> = Vec::new();

    loop {
        if seen.iter().any(|(n, s)| *n == cur_name && *s == cur_section) {
            return Err(format!("circular .so redirect at {cur_name}"));
        }
        if seen.len() >= MAX_SO_DEPTH {
            return Err(format!(".so redirect chain for {name} exceeds {MAX_SO_DEPTH} hops"));
        }
        seen.push((cur_name.clone(), cur_section));

        let e = pick(ents, &cur_name, cur_section, lang).ok_or_else(|| {
            if seen.len() > 1 {
                format!("broken .so redirect — {cur_name} not found")
            } else {
                format!("no embedded manual page for {cur_name}")
            }
        })?;

        match &e.redirect {
            None => return Ok(e.path.clone()),
            Some((tname, tsect)) => {
                cur_name = tname.clone();
                cur_section = Some(*tsect);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ent(path: &str, redirect: Option<(&str, u32)>) -> Entry {
        parse_line(&format!(
            "{path}\t{}",
            redirect.map_or_else(|| "10".to_owned(), |(n, s)| format!("-> {n}.{s}")),
        ))
        .unwrap()
    }

    #[test]
    fn parses_plain_default_lang() {
        let e = parse_line("unpin/man/ls.1\t909").unwrap();
        assert_eq!(e.name, "ls");
        assert_eq!(e.lang, "en");
        assert_eq!(e.section, 1);
        assert_eq!(e.path, "unpin/man/ls.1");
        assert!(e.redirect.is_none());
    }

    #[test]
    fn parses_lang_and_redirect() {
        let e = parse_line("unpin/man/pt_BR/ls.1\t909").unwrap();
        assert_eq!(e.lang, "pt_BR");
        assert_eq!(e.name, "ls");

        let r = parse_line("unpin/man/dir.1\t-> ls.1").unwrap();
        assert_eq!(r.redirect, Some(("ls".to_owned(), 1)));
    }

    #[test]
    fn skips_non_man_lines() {
        assert!(parse_line("unpin/meta/alias\tfoo").is_none());
        assert!(parse_line("unpin/man/a/b/c.1\t9").is_none()); // nested
        assert!(parse_line("no-tab-here").is_none());
    }

    #[test]
    fn picks_lowest_section_then_lang() {
        let ents = [ent("unpin/man/ls.5", None), ent("unpin/man/ls.1", None)];
        assert_eq!(pick(&ents, "ls", None, "en").unwrap().section, 1);
        assert_eq!(pick(&ents, "ls", Some(5), "en").unwrap().section, 5);
        assert!(pick(&ents, "nope", None, "en").is_none());
    }

    #[test]
    fn resolves_so_redirect_and_detects_cycles() {
        let ents = [ent("unpin/man/dir.1", Some(("ls", 1))), ent("unpin/man/ls.1", None)];
        assert_eq!(resolve(&ents, "dir", "en").unwrap(), "unpin/man/ls.1");

        let cyc = [ent("unpin/man/a.1", Some(("b", 1))), ent("unpin/man/b.1", Some(("a", 1)))];
        assert!(resolve(&cyc, "a", "en").is_err());
    }
}
