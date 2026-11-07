{
  description = "Tab5-ADB development environment";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";
    esp-devkit = {
      url = "git+file:./esp-devkit";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.flake-utils.follows = "flake-utils";
    };
  };

  outputs = { self, nixpkgs, flake-utils, esp-devkit }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config = {
            android_sdk.accept_license = true;
            allowUnfree = true;
          };
        };
        androidSdk = (pkgs.androidenv.composeAndroidPackages {
          platformVersions = [ "34" ];
          buildToolsVersions = [ "34.0.0" ];
          includeEmulator = false;
          includeSystemImages = false;
          includeSources = false;
        }).androidsdk;
        androidSdkRoot = "${androidSdk}/libexec/android-sdk";
      in {
        packages.fw-image = esp-devkit.lib.${system}.mkFwImage {
          name = "tab5-adb-fw";
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ esp-devkit.devShells.${system}.default ];
          packages = [ pkgs.jdk androidSdk ];
          shellHook = ''
            export ANDROID_JAR="${androidSdkRoot}/platforms/android-34/android.jar"
            export PATH="$PATH:${androidSdkRoot}/build-tools/34.0.0"
          '';
        };
      }
    );
}
