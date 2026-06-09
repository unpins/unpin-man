{
  description = "The unpin man renderer — renders the manual pages an unpins program carries inside itself (the `unpin man` verb)";

  nixConfig = {
    extra-substituters = [ "https://unpins.cachix.org" ];
    extra-trusted-public-keys = [ "unpins.cachix.org-1:DDaShjbZ8VvcqxeTcAU3kV9vxZQBlyb7V/uLBHfTynI=" ];
  };

  # A plain Rust crate (it replaced a C/Cosmopolitan front-end). Going Rust is
  # what lets it ship a native Windows `.exe` through mingw — no Cosmopolitan —
  # exactly like `unpin` and `unpin-readme`. roff→ANSI rendering links mandoc's
  # render-only subset (vendor/mandoc, compiled by build.rs), which builds under
  # mingw too. This flake mirrors unpins/unpin-readme's flake.nix.
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    unpins-lib.url = "github:unpins/nix-lib";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, unpins-lib, rust-overlay }:
    let
      ulib = unpins-lib.lib;
      nixpkgsFor = ulib.forAllNative (system: import nixpkgs {
        inherit system;
        overlays = [ rust-overlay.overlays.default ];
      });

      version = (nixpkgs.lib.importTOML ./Cargo.toml).package.version;

      src = nixpkgs.lib.cleanSourceWith {
        src = ./.;
        filter = path: _:
          let base = baseNameOf (toString path); in
          !(base == "target" || nixpkgs.lib.hasPrefix "result" base || base == ".github");
      };

      mkPkg = { rustPlatform, env ? {}, auditable ? true }:
        (rustPlatform.buildRustPackage {
          pname = "unpin-man";
          inherit version src auditable env;
          cargoLock.lockFile = ./Cargo.lock;
          doCheck = false;
        }).overrideAttrs (_: { stripAllList = [ "bin" ]; });

      nativePkg = system:
        mkPkg {
          rustPlatform = nixpkgsFor.${system}.pkgsStatic.rustPlatform;
          env.RUSTFLAGS = "-C relocation-model=static";
        };

      # auditable=false: rustc + LTO + cargo-auditable overflows mingw's 32-bit
      # relocation limit. Plain `cargo build --target` skips auditable.
      windowsPkg = mkPkg {
        rustPlatform = nixpkgsFor.x86_64-linux.pkgsCross.mingwW64.rustPlatform;
        auditable = false;
      };

      # rustc injects `-liconv` on darwin; the default cross stdenv ships
      # libiconv as a dylib, which action-build rejects. pkgsStatic.libiconv
      # first on buildInputs makes the linker pick the `.a` and emit no dylib
      # load command. Only libiconv goes static — the rest of the cross stays
      # non-static so the broken cctools/xar-static cascade isn't pulled in.
      darwinX86Pkg =
        let cross = nixpkgsFor.aarch64-darwin.pkgsCross.x86_64-darwin; in
        (mkPkg { rustPlatform = cross.rustPlatform; }).overrideAttrs (old: {
          buildInputs = [ cross.pkgsStatic.libiconv ] ++ (old.buildInputs or [ ]);
          # buildInputs above only covers the target (x86_64) link. But in this
          # arm→x86 cross the proc-macros are compiled for the BUILD host
          # (aarch64) and rustc links each one as a `.dylib` with `-liconv` —
          # and the build→build cc-wrapper has no libiconv in its search path,
          # so that link fails. (unpin only escapes this because its proc-macro
          # dylibs come pre-built from cachix; a cold/new crate links them and
          # breaks.) `depsBuildBuild` did NOT populate the flag, so push the
          # `-L` straight onto the var the build→build wrapper actually reads:
          # `NIX_LDFLAGS_<suffixSalt>`, salt = arm64_apple_darwin (confirmed via
          # `cross.buildPackages.stdenv.cc.suffixSalt`), with the build-arch
          # (aarch64) libiconv — not the x86_64 target one.
          NIX_LDFLAGS_arm64_apple_darwin = "-L${cross.buildPackages.libiconv}/lib";
        });

      # Shared rustup-distributed toolchain: rustc as a native binary plus a
      # precompiled `rust-std-<triple>` per cross target — no source build of
      # cross-rustc. Fetched once across the musl crosses below.
      rustToolchain = pkgs: pkgs.rust-bin.stable.latest.default.override {
        targets = [
          "i686-unknown-linux-musl"
          "armv7-unknown-linux-musleabihf"
          "powerpc64le-unknown-linux-musl"
          "riscv64gc-unknown-linux-musl"
        ];
      };

      # Cross build for musl targets: rust-overlay rustc + rustup `rust-std` +
      # the cross C toolchain in `crossPkgs.stdenv` (for the final link).
      # `crossPkgs.makeRustPlatform` bakes `--target <triple>` and
      # `CC_<TARGET>`/`CARGO_TARGET_<TARGET>_LINKER` into the build hook.
      mkCross = crossPkgs:
        let rust = rustToolchain crossPkgs.buildPackages; in
        mkPkg {
          rustPlatform = crossPkgs.makeRustPlatform { cargo = rust; rustc = rust; };
          auditable = false;
          # rust-overlay's musl specs default to crt-static=false (rustup's
          # convention); the explicit `+crt-static` keeps the binary from
          # carrying a musl dynamic-link interpreter that action-build rejects.
          env.RUSTFLAGS = "-C target-feature=+crt-static";
        };

      linuxI686Pkg    = mkCross nixpkgsFor.x86_64-linux.pkgsCross.musl32;
      linuxPpc64lePkg = mkCross nixpkgsFor.x86_64-linux.pkgsCross.musl-power;

      # riscv64-musl isn't pre-cooked in pkgsCross — spell the triple out.
      linuxRiscv64Pkg = mkCross (import nixpkgs {
        system = "x86_64-linux";
        overlays = [ rust-overlay.overlays.default ];
        crossSystem = { config = "riscv64-unknown-linux-musl"; };
      });

      # Built on the ubuntu-24.04-arm runner, so this lives under aarch64-linux.
      linuxArmv7lPkg = mkCross nixpkgsFor.aarch64-linux.pkgsCross.muslpi;

      nativePackages = ulib.forAllNative (system: { default = nativePkg system; });
    in
    {
      packages = nativePackages // {
        x86_64-linux = nativePackages.x86_64-linux // {
          "windows-x86_64" = windowsPkg;
          "linux-i686" = linuxI686Pkg;
          "linux-ppc64le" = linuxPpc64lePkg;
          "linux-riscv64" = linuxRiscv64Pkg;
        };
        aarch64-linux = nativePackages.aarch64-linux // {
          "linux-armv7l" = linuxArmv7lPkg;
        };
        aarch64-darwin = nativePackages.aarch64-darwin // {
          "darwin-x86_64" = darwinX86Pkg;
        };
      };

      apps = ulib.forAllNative (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/unpin-man";
        };
      });
    };
}
