# unpin-man

A self-contained [mandoc](https://mandoc.bsd.lv/) man renderer, built natively for Linux, macOS, and Windows — it renders the man pages that [unpins](https://unpins.org) programs carry inside themselves. It is the helper behind the `unpin man` verb, fetched on demand and never placed on `PATH`.

[![CI](https://github.com/unpins/unpin-man/actions/workflows/unpin-man.yml/badge.svg)](https://github.com/unpins/unpin-man/actions)
![Linux](https://img.shields.io/badge/Linux-✓-success?logo=linux&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-✓-success?logo=apple&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-✓-success?logo=windows&logoColor=white)

Part of the [unpins](https://unpins.org) project.

## What it is

unpins binaries embed their own man pages (`unpin/man/*` entries in an appended ZIP). `unpin` itself knows nothing about the man format — it only exposes those bytes through `unpin bundle list|dump`. This package is the other half: a Rust program that resolves the right page over `unpin bundle` instead of reading `/usr/share/man` (lowest section, language fallback, `.so` redirects), renders its roff with a vendored, in-process mandoc, and pages it. No companion files, no network, no system `man(1)` required.

## Usage

```bash
unpin man                   # no package → unpin's own manual
unpin man coreutils ls      # render coreutils' embedded ls(1)
unpin man jq                # the page named like the package
```

`unpin man` fetches and runs this package on demand (cached after first use, never linked onto `PATH` — it's a verb, not a command you install, so it can't shadow your system `man(1)`). With no package name it defaults to `unpin`, so a bare `unpin man` shows unpin's own manual.

Run directly it behaves the same — `unpin-man <pkg> [page]`, `unpin-man -` to read roff from stdin, plus `--help` / `--version`. It finds unpin via `$UNPIN_SELF` (exported by `unpin run`/`unpin man`), falling back to `unpin` on `$PATH`.

## Build locally

```bash
nix build github:unpins/unpin-man
UNPIN_SELF="$(command -v unpin)" ./result/bin/unpin-man unpin
```

The first invocation will offer to add the [unpins.cachix.org](https://unpins.cachix.org) substituter so most pulls come pre-built.

## How it works

A plain Rust crate, in three parts:

- **`src/main.rs`** — bundle resolution. Enumerates the package's embedded `unpin/man/*` pages via `unpin bundle list`, picks the right *(name, section, lang)*, follows whole-page `.so` redirects, and dumps the chosen entry's roff with `unpin bundle dump`.
- **`src/mandoc.rs`** + **`vendor/mandoc/`** — rendering, **in-process**. A trimmed mandoc render-only subset (parse + terminal-format closure) is compiled by `build.rs` and called through a one-function C bridge: roff bytes go in, rendered UTF-8 comes back, which Rust converts from nroff overstrike to ANSI SGR. No subprocess, no temp files — the whole render happens in memory (`mparse_readmem` in, a growable buffer out), which is also what lets it work on a native Windows build.
- **`src/render.rs`** — a crossterm pager that reflows on resize (re-rendering at the new width), with a plain non-tty fallback so `unpin man pkg | grep` keeps working.

## Build notes

- **Native Windows via mingw, no Cosmopolitan.** Being a plain Rust crate that links mandoc's render-only subset (which builds under mingw) is what lets it ship a real `.exe` — unlike the earlier C front-end, which needed cosmo's libc to `fork`/`exec` a separate mandoc.
- **`build.rs` synthesises `config.h`.** mandoc's `./configure` compiles and runs probe programs to fill `config.h`, which can't work when cross-compiling. Instead `build.rs` emits the right `HAVE_*` per libc family (glibc / musl / darwin / mingw) and compiles each bundled `compat_*.c` only when that libc lacks the function.
- **Cross builds.** The flake mirrors `unpins/unpin-readme`: a native static-musl build per platform, mingw for Windows, and musl crosses for i686 / ppc64le / riscv64 / armv7l, plus an x86_64-darwin cross.
- **No embedded man of its own.** It ships only the renderer; the pages it shows belong to *other* packages, so `unpin man man` has nothing to display.
