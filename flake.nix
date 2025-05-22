# Copyright 2024, Gavin John <gavinnjohn@gmail.com>
# SPDX-License-Identifier: CC0-1.0 OR MIT OR BSL-1.0

{
  inputs = {
    # Whenever an upstream change is merged, update this and
    # remove the packages from the ...ToUpstream lists below
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs =
    inputs@{ nixpkgs, flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      perSystem =
        { pkgs, lib, ... }:
        let
          devTools = with pkgs; [
            # Tools that are required in order to develop with Monado
            # but are not required to build Monado itself
            clang-tools
            cmake-format
            git
            gradle
            gradle-completion
          ];

          nativeBuildInputsToUpstream = with pkgs; [
            # If there are any nativeBuildInputs that are not in nixpkgs, add them here
            # nativeBuildInputs are packages that are needed to develop and/or build the project (i.e. tooling)
          ];

          buildInputsToUpstream = with pkgs; [
            # If there are any buildInputs that are not in nixpkgs, add them here
            # buildInputs are any packages that are needed at runtime (i.e. dependencies)
          ];

          package = pkgs.monado.overrideAttrs (oldAttrs: {
            src = ./.;

            nativeBuildInputs = oldAttrs.nativeBuildInputs ++ nativeBuildInputsToUpstream;
            buildInputs = oldAttrs.buildInputs ++ buildInputsToUpstream;
            cmakeFlags = lib.remove "-DXRT_HAVE_STEAM:BOOL=TRUE" oldAttrs.cmakeFlags;

            patches = [ ];
          });
        in
        {
          packages.default = package;
          devShells.default = package.overrideAttrs (oldAttrs: {
            nativeBuildInputs = oldAttrs.nativeBuildInputs ++ devTools;
          });

          formatter = pkgs.nixfmt-rfc-style;
        };
    };
}
