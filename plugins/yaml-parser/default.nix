{
  cargo,
  lld,
  lib,
  rustPlatform,
  nix-gitignore,
  stdenv
}:
let
  src = nix-gitignore.gitignoreSource [] ./.;
  crate = "${src}/Cargo.toml" |> builtins.readFile  |> builtins.fromTOML;

  cargoDeps = rustPlatform.fetchCargoVendor {
    inherit src;
    hash = "sha256-DDw6TO1LDk/rgucCFjiWg/mmpoCvVVNdZ8WvupaIdWA=";
  };
in
# Forgive me.
# This is just the easiest way I could conceive to compile to Wasm without having
# to build an entire clang toolchain etc.
stdenv.mkDerivation (finalAttrs: {
  pname = crate.package.name;
  version = crate.package.version;

  nativeBuildInputs = [
    cargo
    lld
  ];

  inherit src;
  unpackPhase = ''
    runHook preUnpack;

    cp -r $src/. .

    runHook postUnpack;
  '';
  configurePhase = ''
    runHook preConfigure;

    mkdir -p .cargo
    cat > .cargo/config.toml << EOF
    [source.crates-io]
    replace-with = "vendored-sources"

    [source.vendored-sources]
    directory = "vendor"
    EOF

    runHook postConfigure;
  '';
  buildPhase = ''
    runHook preBuild;

    ln -sf ${cargoDeps} vendor
    cargo build --release --target wasm32-unknown-unknown --offline

    runHook postBuild;
  '';

  installPhase = ''
    runHook preInstall;

    mkdir -p $out/lib
    cp target/wasm32-unknown-unknown/release/${finalAttrs.pname |> lib.replaceStrings ["-"] ["_"]}.wasm $out/lib/plugin.wasm

    runHook postInstall;
  '';
})
