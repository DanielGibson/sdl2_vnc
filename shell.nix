let
  nixpkgs = import <nixpkgs> {};
  pkg = nixpkgs.callPackage ./. {};
  tools = with nixpkgs; [ gdb valgrind clang-tools ];

in
  pkg.overrideAttrs (old: { buildInputs = old.buildInputs ++ tools; })
