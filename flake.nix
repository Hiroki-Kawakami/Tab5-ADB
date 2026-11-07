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
        emulator = api:
          let
            abi = if pkgs.stdenv.hostPlatform.isAarch64 then "arm64-v8a" else "x86_64";
            sdk = (pkgs.androidenv.composeAndroidPackages {
              platformVersions = [ api ];
              includeEmulator = true;
              includeSystemImages = true;
              systemImageTypes = [ "google_apis" ];
              abiVersions = [ abi ];
            }).androidsdk;
            name = "android-${api}";
          in pkgs.writeShellApplication {
            name = "emulator-${api}";
            text = ''
              home="''${TAB5_ADB_EMULATOR_HOME:-$PWD/.emulator}"
              export ANDROID_HOME="${sdk}/libexec/android-sdk"
              export ANDROID_SDK_ROOT="$ANDROID_HOME"
              export ANDROID_USER_HOME="$home"
              export ANDROID_AVD_HOME="$home/avd"
              if [ ! -d "$ANDROID_AVD_HOME/${name}.avd" ]; then
                mkdir -p "$ANDROID_AVD_HOME"
                tag=$(ls "$ANDROID_HOME/system-images/android-${api}")
                echo no | ${sdk}/bin/avdmanager create avd -n "${name}" \
                  -k "system-images;android-${api};$tag;${abi}" -p "$ANDROID_AVD_HOME/${name}.avd"
              fi
              exec "$ANDROID_HOME/emulator/emulator" -avd "${name}" -port 5554 -no-boot-anim "$@"
            '';
          };
      in {
        packages = {
          fw-image = esp-devkit.lib.${system}.mkFwImage {
            name = "tab5-adb-fw";
          };
          emulator-28 = emulator "28";
          emulator-31 = emulator "31";
          emulator-36 = emulator "36";
          emulator-37 = emulator "37.0";
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
