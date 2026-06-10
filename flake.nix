{
  description = "ESP32 development environment";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
    flake-utils.url = "github:numtide/flake-utils";
    esp-dev = {
      url = "github:mirrexagon/nixpkgs-esp-dev";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.flake-utils.follows = "flake-utils";
    };
  };

  outputs = { self, nixpkgs, flake-utils, esp-dev }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ esp-dev.overlays.default ];
          config = {
            permittedInsecurePackages = [
              "python3.13-ecdsa-0.19.1"
            ];
            android_sdk.accept_license = true;
            allowUnfree = true;  # Android SDK (android-agent/ build) is unfree
          };
        };
        esp-idf = pkgs.esp-idf-full.override {
          rev = "v5.4.3";
          sha256 = "sha256-sV/eL3jRG9GdaQNByBypmH5ZKmZoOnWCEY1ABySIeac=";
        };
        # Android side companion (android-agent/): SDK for android.jar + d8.
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
          inputsFrom = [ esp-idf ];
          packages = [
            esp-idf
            # Host simulator toolchain (simulator/)
            pkgs.cmake
            pkgs.ninja
            pkgs.gcc
            pkgs.ccache
            pkgs.cjson
            pkgs.SDL2
            pkgs.libusb1   # embedded_adb host transport (simulator)
            pkgs.mbedtls   # embedded_adb RSA auth (simulator); device gets it from ESP-IDF
            pkgs.libjpeg   # agent_link host test: decode the JPEG strips (device uses P4 HW JPEG)
            pkgs.zlib      # screencap PNG preview inflate (simulator); device gets espressif/zlib via idf_component.yml
            # android-agent/ toolchain
            pkgs.jdk        # javac for the Java agent
            pkgs.android-tools  # standalone adb (push/shell/forward) for the dev loop
            androidSdk      # android.jar (compile) + d8 (dex)
          ];
          shellHook = ''
            export ESP_IDF_VERSION="5.4"
            export HOST_GCC="${pkgs.gcc}"
            export ANDROID_SDK_ROOT="${androidSdkRoot}"
            export ANDROID_JAR="${androidSdkRoot}/platforms/android-34/android.jar"
            export PATH="$PATH:${androidSdkRoot}/build-tools/34.0.0"
          '';
        };
      }
    );
}
