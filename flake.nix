{
  description = "ESP32 development environment";

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
            permittedInsecurePackages = [
              "python3.13-ecdsa-0.19.1"
            ];
            android_sdk.accept_license = true;
            allowUnfree = true;
          };
        };
        androidComposition = pkgs.androidenv.composeAndroidPackages {
          platformVersions = [ "34" ];
          buildToolsVersions = [ "34.0.0" ];
          includeEmulator = false;
          includeSystemImages = false;
          includeSources = false;
        };
        androidSdk = androidComposition.androidsdk;
        androidSdkRoot = "${androidSdk}/libexec/android-sdk";
      in {
        devShells.default = pkgs.mkShell {
          inputsFrom = [ esp-devkit.devShells.${system}.default ];
          packages = [
            pkgs.libusb1
            pkgs.mbedtls_4
            pkgs.boringssl
            pkgs.jdk
            pkgs.android-tools
            androidSdk
          ];
          shellHook = ''
            unset IDF_TOOLS_PATH
            export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
            export ANDROID_SDK_ROOT="${androidSdkRoot}"
            export ANDROID_JAR="${androidSdkRoot}/platforms/android-34/android.jar"
            export PATH="$PATH:${androidSdkRoot}/build-tools/34.0.0"
          '';
        };
      }
    );
}
