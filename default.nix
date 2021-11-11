{ stdenv
, SDL2
, patchelf
}:

stdenv.mkDerivation {
  pname = "SDL2_vnc";
  version = "0.0.1";

  src = ./.;

  buildInputs = [ SDL2 patchelf ];
  makeFlags = [ "PREFIX=$(out)" ];

  postInstall = ''
    patchelf $out/bin/vncc \
      --set-rpath "$(patchelf --print-rpath $out/bin/vncc):$out/lib"
  '';
}
