# unpin-man

Standalone build of [mandoc](https://mandoc.bsd.lv/), patched to render the man pages that [unpins](https://unpins.org) programs carry inside themselves. It is the helper behind the `unpin man` verb — fetched on demand, never placed on `PATH`.

[![CI](https://github.com/unpins/unpin-man/actions/workflows/unpin-man.yml/badge.svg)](https://github.com/unpins/unpin-man/actions)
![Linux](https://img.shields.io/badge/Linux-✓-success?logo=linux&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-✓-success?logo=apple&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-✓-success?logo=windows&logoColor=white)

Part of the [unpins](https://unpins.org) project — native single-binary builds with no third-party runtime dependencies.

## What it is

unpins binaries embed their own man pages (`unpin/man/*` entries in an appended ZIP). `unpin` itself knows nothing about the man format — it only exposes those bytes through `unpin bundle list|dump`. This package is the other half: a mandoc whose front-end shells back to `unpin bundle` instead of reading `/usr/share/man`, picks the right page (lowest section, language fallback, `.so` redirects), and hands the roff to mandoc's renderer. No companion files, no network, no system `man(1)` required.

## Usage

```bash
unpin man                   # no package → unpin's own manual
unpin man coreutils ls      # render coreutils' embedded ls(1)
unpin man jq                # the page named like the package
```

`unpin man` fetches and runs this package on demand (cached after first use, never linked onto `PATH` — it's a verb, not a command you install, so it can't shadow your system `man(1)`). With no package name it defaults to `unpin`, so a bare `unpin man` shows unpin's own manual.

## Build locally

```bash
nix build github:unpins/unpin-man
UNPIN_SELF="$(command -v unpin)" ./result/bin/unpin-man unpin
```

The first invocation will offer to add the [unpins.cachix.org](https://unpins.cachix.org) substituter so most pulls come pre-built.

## Build notes

- **Front-end, not a `man(1)` clone.** `unpin-front-end.patch` renames mandoc's `main` to `mandoc_main` and relinks the `man` target around `unpin_man.c`, which resolves the page over `unpin bundle list|dump` and feeds the roff to `mandoc_main` on stdin. None of mandoc's `/usr/share/man` search, `apropos`, or `makewhatis` machinery is reached. Invoked directly as `man [pkg] [page]`, it finds unpin via `$UNPIN_SELF` (exported by `unpin run`/`unpin man`), else `unpin` on `$PATH`.
- **No `--version` / no smoke test.** A lone argument is read as a package name, so there is no non-interactive probe to assert on.
- **No embedded man of its own.** It ships only the front-end; the pages it renders belong to *other* packages, so `unpin man man` has nothing to show.
- **Windows uses [Cosmopolitan](https://justine.lol/cosmopolitan/) (cosmocc), not mingw.** The front-end shells back to `unpin` with POSIX `fork`/`exec`/`pipe`, which mingw's CRT lacks but cosmo's libc provides. See `cosmo.nix`.
- **`doCheck = false`.** mandoc's `regress` suite rebuilds the stock `mandoc` target, which no longer has a `main` after the patch.
- **Cross builds.** mandoc's `./configure` runs probe binaries, so nixpkgs marks it broken for build≠host. For the targets the build box can't execute (ppc64le, riscv64, armv7l) we preset `HAVE_*` in `configure.local` from a native musl probe; i686, the Rosetta'd x86_64-darwin, and the cosmo APE probes run for real. See the comments in `flake.nix` / `cosmo.nix`.
