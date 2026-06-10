{
  description = "The unpin man renderer — renders the manual pages an unpins program carries inside itself (the `unpin man` verb)";

  nixConfig = {
    extra-substituters = [ "https://unpins.cachix.org" ];
    extra-trusted-public-keys = [ "unpins.cachix.org-1:DDaShjbZ8VvcqxeTcAU3kV9vxZQBlyb7V/uLBHfTynI=" ];
  };

  inputs = {
    unpins-lib.url = "github:unpins/nix-lib";
    # Consumed by mkRustCrate: rustup-distributed rust-std for the cross-musl
    # targets, so no cross rustc is ever built from source.
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "unpins-lib/nixpkgs";
    };
  };

  # A Rust crate (it replaced a C/Cosmopolitan front-end), built by nix-lib's
  # mkRustCrate in own-source mode. `vendoredC`: the roff→ANSI rendering links
  # mandoc's render-only subset (vendor/mandoc, compiled by build.rs via the
  # cc crate), so the musl crosses go through the catalog-cached C cross
  # scopes instead of the chain-free lld path. That C subset builds under
  # mingw too, which is what ships the native Windows `.exe`.
  outputs = { self, unpins-lib, rust-overlay }:
    let nlib = unpins-lib.inputs.nixpkgs.lib; in
    unpins-lib.lib.mkRustCrate {
      inherit self rust-overlay;
      name = "unpin-man";
      own_software = true;
      vendoredC = true;

      version = (nlib.importTOML ./Cargo.toml).package.version;
      cargoLock = ./Cargo.lock;
      src = nlib.cleanSourceWith {
        src = ./.;
        filter = path: _:
          let base = baseNameOf (toString path); in
          !(base == "target" || nlib.hasPrefix "result" base || base == ".github");
      };

      smoke = [ "--version" ];
      smokePattern = "unpin-man [0-9]";
    };
}
