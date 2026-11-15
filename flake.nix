# SPDX-License-Identifier: GPL-3.0-only
{
  description = "LinHT launcher and platform: dev shell for PC builds, tests and the simulator";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { nixpkgs, ... }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
    in {
      devShells.x86_64-linux.default = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
          cmake
          ninja
          pkg-config
          dbus # dbus-daemon / dbus-run-session for the mock services
          (python3.withPackages (ps: [ ps.dbus-next ps.pyyaml ]))
        ];
        # Same libraries the device image provides (sd-bus, libyaml, SQLite); SDL2 for the simulator.
        buildInputs = with pkgs; [ systemdLibs libyaml sqlite SDL2 ];
      };
    };
}
