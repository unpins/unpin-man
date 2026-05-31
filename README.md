# man

Standalone build of [mandoc](https://mandoc.bsd.lv/), patched to render the manuals that [unpins](https://unpins.org) binaries carry inside themselves.

[![CI](https://github.com/unpins/man/actions/workflows/man.yml/badge.svg)](https://github.com/unpins/man/actions)
![Linux](https://img.shields.io/badge/Linux-✓-success?logo=linux&logoColor=white)
![macOS](https://img.shields.io/badge/macOS-✓-success?logo=apple&logoColor=white)

Part of the [unpins](https://unpins.org) project — native single-binary builds with no third-party runtime dependencies.

## What it is

unpins binaries embed their own man pages (as `unpin/man/*` entries in an
appended ZIP — see the unpin docs). `unpin` itself knows nothing about the man
format; it only exposes those bytes through `unpin bundle list|dump`. This
package is the other half: a mandoc whose front-end shells back to
`unpin bundle` instead of reading `/usr/share/man`, picks the right page (lowest
section, language fallback, `.so` redirects), and hands the roff to mandoc's
own renderer. So `unpin man <pkg> [page]` formats a package's embedded manual
with no companion file and no network — including on systems with no `man(1)`.

## Installation

Install with [unpin](https://github.com/unpins/unpin):

```bash
unpin man
```

Then read any installed package's embedded manual:

```bash
unpin man coreutils ls      # render coreutils' embedded ls(1)
unpin man jq                # the page named like the package
```

Invoked directly (`man <pkg> [page]`), the binary finds unpin via `$UNPIN_SELF`
(exported by `unpin run`/`unpin man`) and otherwise falls back to `unpin` on
`$PATH`.

## Build locally

```bash
nix build github:unpins/man
UNPIN_SELF="$(command -v unpin)" ./result/bin/man unpin
```

The first invocation will offer to add the [unpins.cachix.org](https://unpins.cachix.org) substituter so most pulls come pre-built.

## Manual download

The [Releases](https://github.com/unpins/man/releases) page has standalone binaries for manual download.

## Build notes

- **Front-end, not a `man(1)` clone.** `unpin-front-end.patch` renames mandoc's
  `main` to `mandoc_main` and relinks the `man` target around `unpin_man.c`,
  which resolves the page over `unpin bundle list|dump` and feeds the roff to
  `mandoc_main` on stdin. None of mandoc's `/usr/share/man` search, `apropos`,
  or `makewhatis` machinery is reached.
- **No `--version` / no smoke test.** A lone argument is read as a package name,
  so there is no quick non-interactive probe to assert on.
- **No embedded man of its own.** Every other package embeds its upstream man
  pages (`embedMan`); this one ships only the front-end binary, so `unpin man
  man` has nothing to show. The pages it renders belong to *other* packages.
- **Windows excluded for now.** The front-end uses POSIX `fork`/`exec`/`pipe`
  to shell back to `unpin`, which mingw's CRT lacks. Cosmopolitan's libc has
  `fork`, so a cosmocc build is the likely route — deferred.
- **`doCheck = false`.** mandoc's `regress` suite rebuilds the stock `mandoc`
  target, which no longer has a `main` after the patch.
- **Cross builds.** mandoc's `./configure` runs probe binaries, so nixpkgs marks
  it broken for build≠host. For the targets the build box can't execute
  (ppc64le, riscv64, armv7l) we preset `HAVE_*` in `configure.local` from a
  native musl probe so configure runs nothing; i686 and the Rosetta'd
  x86_64-darwin run the real probes. See the comment in `flake.nix`.
