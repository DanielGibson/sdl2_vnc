let
  nixpkgs-tar = fetchTarball "https://github.com/NixOS/nixpkgs/archive/4b7e51865e44d3afc564e3a3111af12be1003251.tar.gz";
  nixpkgs = import nixpkgs-tar {};
  pkg = nixpkgs.callPackage ./. {};
  tools = with nixpkgs; [ gdb valgrind clang-tools ];

in
  pkg.overrideAttrs (old: { buildInputs = old.buildInputs ++ tools; })
