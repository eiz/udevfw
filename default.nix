let
  pkgs = import <nixpkgs> {};
  stdenv = pkgs.stdenv;
in
stdenv.mkDerivation rec {
  name = "udevfw";
  src = ./.;
  buildInputs = [ stdenv pkgs.udev ];
}
