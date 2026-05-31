# man (patched mandoc) via cosmoStaticCross for Windows-x86_64.
#
# Why cosmo and not mingw: the front-end (unpin_man.c) shells back to
# `unpin bundle list|dump` with POSIX fork/exec/pipe to stream the roff.
# mingw's CRT has none of those; Cosmopolitan's libc does (it emulates
# fork on Windows), so the same front-end source builds unchanged.
#
# Unlike the Linux/macOS cross targets (see flake.nix), this build needs
# NO configure.local preset: mandoc's ./configure compiles and runs probe
# programs to fill config.h, and cosmocc emits APE binaries that execute
# natively on the x86_64-linux build box. So configure runs the real cosmo
# probes (correctly detecting NEED_GNU_SOURCE, HAVE_WCHAR, …) — same as
# coreutils' autoconf configure does under this stdenv.
#
# ELF → PE32+ rename to `man.exe` happens automatically via the cosmo cross
# stdenv's apelink setup hook in fixupPhase; installPhase ships plain `man`.
{ unpins-lib }:
pkgs:
let
  cosmoPkgs = unpins-lib.lib.cosmoStaticCross pkgs;
in
cosmoPkgs.mandoc.overrideAttrs (old: {
  pname = "man";
  # nixpkgs' mandoc carries zlib in buildInputs, but the cosmo cross zlib is a
  # split-output drv whose buildInputs slot is the `dev` output (headers +
  # pkgconfig only) — `libz.a` lives in the separate `static` output, off the
  # link path. So configure detects zlib (HAVE_ZLIB=1, matching every other
  # target) and emits `-lz`, then ld can't find it. Add the static output so
  # `-L…-static/lib` reaches the link line; zlib stays enabled, no config drift.
  # (pkgsStatic on Linux/macOS keeps `libz.a` in `out`, so this is cosmo-only.)
  buildInputs = (old.buildInputs or [ ]) ++ [ cosmoPkgs.zlib.static ];
  patches = (old.patches or [ ]) ++ [ ./unpin-front-end.patch ];
  postPatch = (old.postPatch or "") + ''
    cp ${./unpin_man.c} unpin_man.c
  '';
  # Build ONLY the `man` target: the stock `mandoc` target links main.o,
  # whose `main` the patch renamed, and would fail to link.
  buildFlags = (old.buildFlags or [ ]) ++ [ "man" ];
  # regress rebuilds the `mandoc` target (no `main` now) and we ship only
  # the front-end anyway.
  doCheck = false;
  installPhase = ''
    runHook preInstall
    install -Dm755 man $out/bin/man
    runHook postInstall
  '';
  # nixpkgs' broken = (build.system != host.system) is too coarse for the
  # cosmo cross, whose probes run on the build box. Lift it.
  meta = (old.meta or { }) // { broken = false; };
})
