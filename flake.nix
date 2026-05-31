{
  description = "Standalone build of man — a patched mandoc that renders unpins' embedded manuals";

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
      name = "man";
      # nixpkgs ships the renderer as `mandoc`; we ship it as `man` because
      # `unpin man <pkg>` dispatches the verb to the package of the same name.
      pkgsAttr = "mandoc";

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
          needsPreset = !(stdenv.buildPlatform.canExecute stdenv.hostPlatform);
        in
        pkgs.pkgsStatic.mandoc.overrideAttrs (old: {
          pname = "man";
          patches = (old.patches or [ ]) ++ [ ./unpin-front-end.patch ];
          postPatch = (old.postPatch or "") + ''
            cp ${./unpin_man.c} unpin_man.c
          '';
          # On a target the build box can't run, preset HAVE_* from a native
          # musl probe so ./configure executes nothing (see probeConfigH).
          # NEED_GNU_SOURCE is a singletest side effect, not a config.h HAVE_
          # flag, so it isn't harvested above — but presetting HAVE_* skips the
          # test that would set it, and musl gates strcasestr/strndup/… (used by
          # mandoc) behind _GNU_SOURCE. Set it explicitly so config.h emits
          # `#define _GNU_SOURCE` and those decls are visible.
          preConfigure = (old.preConfigure or "") + lib.optionalString needsPreset ''
            sed -n 's/^#define \(HAVE_[A-Z0-9_]*\) \([0-9]*\)$/\1=\2/p' \
              ${probeConfigH "x86_64-linux"}/config.h >> configure.local
            echo 'NEED_GNU_SOURCE=1' >> configure.local
          '';
          # Build ONLY the `man` target: the stock `mandoc` target links
          # main.o, whose `main` we renamed, and would fail to link.
          buildFlags = (old.buildFlags or [ ]) ++ [ "man" ];
          # The regress suite rebuilds the `mandoc` target (no `main` now → a
          # link error), and we ship only the front-end anyway.
          doCheck = false;
          installPhase = ''
            runHook preInstall
            install -Dm755 man $out/bin/man
            runHook postInstall
          '';
          # nixpkgs' broken = (build.system != host.system) is too coarse for a
          # same-libc pkgsStatic cross; we make every such target build (above).
          meta = (old.meta or { }) // { broken = false; };
        });
    };
}
