{
  description = "The unpin man renderer — a patched mandoc that renders the manuals unpins programs carry inside themselves";

  nixConfig = {
    extra-substituters = [ "https://unpins.cachix.org" ];
    extra-trusted-public-keys = [ "unpins.cachix.org-1:DDaShjbZ8VvcqxeTcAU3kV9vxZQBlyb7V/uLBHfTynI=" ];
  };

  inputs.unpins-lib.url = "github:unpins/nix-lib";

  outputs = { self, unpins-lib }:
    let
      # mandoc's ./configure COMPILES AND RUNS little probe programs to fill
      # config.h's HAVE_* flags, so nixpkgs marks it `broken` whenever build ≠
      # host (it can't run the target's binaries) — which would kill every cross
      # target. But pkgsStatic is a *same-libc* cross: the probe results are
      # identical to a real native build of that libc. So on a target the build
      # box can't execute, we harvest the HAVE_* from a native build of the same
      # libc and preset them in configure.local; ./configure then skips every
      # test (the `ismanual` short-circuit) and cross-compiles cleanly, keeping
      # the correct values (notably HAVE_WCHAR=1 → UTF-8 output). Deriving the
      # values from a real build means they track upstream automatically — no
      # hand-maintained HAVE_* list to rot on a mandoc bump.
      #
      # Targets the build box CAN execute (i686 + native on x86_64; x86_64-darwin
      # under Rosetta on aarch64-darwin) run the real probes and need no preset —
      # only the conservative `broken` flag lifted.
      probeConfigH = system:
        (import unpins-lib.inputs.nixpkgs { inherit system; }).pkgsStatic.mandoc.overrideAttrs (_: {
          doCheck = false;
          dontBuild = true;
          installPhase = "mkdir -p $out; cp config.h $out/config.h";
        });
    in
    unpins-lib.lib.mkStandaloneFlake {
      inherit self;
      name = "unpin-man";
      # This is the `unpin man` verb's helper, so it's named `unpin-man` per the
      # helper-verb convention (docs/helper-verbs.md): reached only via
      # `unpin man`, fetched on demand, never linked onto PATH. mandoc's build
      # target is `man` (buildFlags below); we install it as `unpin-man` so the
      # binary, the published asset, and the package name all agree — action-build
      # locates the primary at `bin/<name>`, so `binName` must match `name`.
      pkgsAttr = "mandoc";

      # Windows goes through Cosmopolitan, not mingw: the front-end shells
      # back to `unpin` with POSIX fork/exec/pipe, which mingw's CRT lacks
      # but cosmo's libc provides. Recipe in ./cosmo.nix.
      windowsBuild = import ./cosmo.nix { inherit unpins-lib; };

      # `man` is a front-end, not a documented tool — `man --version` would be
      # read as a package name. No quick non-interactive probe, so no smoke.

      # unpin knows nothing about man. This package is the other half: a mandoc
      # whose front-end (unpin_man.c) shells back to `unpin bundle list|dump`
      # for a binary's embedded `unpin/man/*` pages instead of reading
      # /usr/share/man, then hands the roff to mandoc's own renderer. The
      # unpin-front-end.patch renames mandoc's `main` to `mandoc_main` and
      # relinks the `man` target around our front-end (see unpin_man.c).
      build = pkgs:
        let
          inherit (pkgs) lib stdenv;
          # Preset only when the build box can't run the target's binaries AND
          # the target is Linux: the harvested config.h comes from a native
          # musl build on the BUILD platform (probeConfigH, below). The HAVE_*
          # are libc features, identical across Linux musl arches, so any Linux
          # build platform is right for any Linux cross (same OS, same libc) —
          # but WRONG for a darwin host (e.g. HAVE_ENDIAN=1 → dbm.c includes a
          # <endian.h> macOS doesn't have). The darwin-x86_64 cross
          # (canExecute=false from an aarch64-darwin runner) instead runs the
          # real probes under Rosetta, which is what the native x86_64-darwin
          # build does too.
          #
          # Key the probe to the build platform, NOT a fixed system: CI cross-
          # builds ppc64le/riscv64 on x86_64-linux runners but armv7l on an
          # aarch64-linux runner (nix-lib gates "linux-armv7l" on aarch64). A
          # hardcoded x86_64-linux probe is unbuildable on the arm runner — it
          # only ever passed by *substituting* the path the x86_64 jobs push to
          # cachix, a cross-job cache race that fails whenever the armv7l job
          # runs ahead of them. buildPlatform.system builds the probe natively
          # on whichever runner the job lands on.
          needsPreset = stdenv.hostPlatform.isLinux
            && !(stdenv.buildPlatform.canExecute stdenv.hostPlatform);
        in
        pkgs.pkgsStatic.mandoc.overrideAttrs (old: {
          pname = "man";
          patches = (old.patches or [ ]) ++ [ ./unpin-front-end.patch ];
          postPatch = (old.postPatch or "") + ''
            cp ${./unpin_man.c} unpin_man.c
          '';
          # On a target the build box can't run, preset HAVE_* from a native
          # musl probe so ./configure executes nothing (see probeConfigH).
          preConfigure = (old.preConfigure or "") + lib.optionalString needsPreset ''
            sed -n 's/^#define \(HAVE_[A-Z0-9_]*\) \([0-9]*\)$/\1=\2/p' \
              ${probeConfigH stdenv.buildPlatform.system}/config.h >> configure.local
            # mandoc's configure only writes a `#define HAVE_X` to config.h when
            # X is ABSENT (to emit a compat shim); a present feature leaves no
            # trace, so the harvest above can't see it. Pin the ones musl always
            # provides — without these the cross probe compiles but can't exec,
            # falls to HAVE_X=0, and config.h diverges from the native build:
            #   NANOSLEEP — a "required function"; 0 is a hard `exit 1`.
            #   O_DIRECTORY / PATH_MAX / ATTRIBUTE — 0 emits a spurious shim
            #     (#define O_DIRECTORY 0, PATH_MAX 4096, __attribute__(x)) that
            #     the native build doesn't, so the config wouldn't match.
            # NEED_GNU_SOURCE is a singletest side effect (not a config.h HAVE_
            # flag), so it isn't harvested either; musl gates strcasestr/strndup
            # behind _GNU_SOURCE, so set it to emit `#define _GNU_SOURCE`.
            for kv in HAVE_NANOSLEEP=1 HAVE_O_DIRECTORY=1 HAVE_PATH_MAX=1 \
                      HAVE_ATTRIBUTE=1 NEED_GNU_SOURCE=1; do
              echo "$kv" >> configure.local
            done
          '';
          # Build ONLY the `man` target: the stock `mandoc` target links
          # main.o, whose `main` we renamed, and would fail to link.
          buildFlags = (old.buildFlags or [ ]) ++ [ "man" ];
          # The regress suite rebuilds the `mandoc` target (no `main` now → a
          # link error), and we ship only the front-end anyway.
          doCheck = false;
          installPhase = ''
            runHook preInstall
            install -Dm755 man $out/bin/unpin-man
            runHook postInstall
          '';
          # nixpkgs' broken = (build.system != host.system) is too coarse for a
          # same-libc pkgsStatic cross; we make every such target build (above).
          meta = (old.meta or { }) // { broken = false; };
        });
    };
}
