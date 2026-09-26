{
  description = "Hyprland plugin: image or video instead of the no_screen_share black box (Rust core)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # packages.default builds against this Hyprland. Point it at yours so they match:
    #   inputs.noshare-cover.inputs.hyprland.follows = "hyprland";
    # (no nixpkgs.follows here: Hyprland then comes prebuilt from hyprland.cachix.org)
    hyprland.url = "github:hyprwm/Hyprland";
  };

  outputs =
    {
      self,
      nixpkgs,
      hyprland,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The plugin must be built against the same headers as the running
      # Hyprland (otherwise it refuses to load), so hyprland is a parameter.
      mkNoshareCover =
        pkgs: hyprlandPkg:
        pkgs.hyprlandPlugins.mkHyprlandPlugin {
          hyprland = hyprlandPkg;
          pluginName = "noshare-cover";
          version = "2.0.12";
          src = self;

          # Rust deps from Cargo.lock, no network needed in the sandbox
          cargoDeps = pkgs.rustPlatform.importCargoLock { lockFile = ./Cargo.lock; };
          nativeBuildInputs = [
            pkgs.cargo
            pkgs.rustc
            pkgs.rustPlatform.cargoSetupHook
            pkgs.nasm # rav1d asm kernels (AV1 on CPU)
            pkgs.rustPlatform.bindgenHook # cros-libva generates libva bindings
          ];
          # only for the VA-API helper (vaapi-helper); not linked into the plugin itself
          buildInputs = [
            pkgs.libva
            pkgs.libgbm
          ];

          # openh264 and libvpx are dlopen-ed; on NixOS they can't be found by
          # soname, so we embed store paths (the plugin works without them,
          # just without H.264/VP9 on CPU)
          env = {
            NSC_LIB_OPENH264 = "${pkgs.openh264}/lib/libopenh264.so";
            NSC_LIB_VPX = "${pkgs.libvpx}/lib/libvpx.so";
          };

          # otherwise make treats a local .so in the tree as an up-to-date build
          preBuild = ''
            rm -f libnoshare-cover.so
          '';
          makeFlags = [ "prefix=${placeholder "out"}" ];

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            cargo test --release --locked --offline
            runHook postCheck
          '';

          meta = {
            description = "Image or video instead of the no_screen_share black box";
            homepage = "https://github.com/gitscout-bot/noshare-cover";
            license = nixpkgs.lib.licenses.bsd3;
          };
        };
    in
    {
      packages = forAll (pkgs: {
        # Hyprland from the hyprwm flake (the `hyprland` input), what most Hyprland-on-Nix
        # setups run
        default = mkNoshareCover pkgs hyprland.packages.${pkgs.stdenv.hostPlatform.system}.hyprland;
        # Hyprland from nixpkgs (programs.hyprland.enable without the hyprwm flake)
        nixpkgs = mkNoshareCover pkgs pkgs.hyprland;
        hyprland-git = self.packages.${pkgs.stdenv.hostPlatform.system}.default; # old name
      });

      # pkgs.hyprlandPlugins.noshare-cover against final.hyprland
      overlays.default = final: prev: {
        hyprlandPlugins = prev.hyprlandPlugins // {
          noshare-cover = mkNoshareCover final final.hyprland;
        };
      };

      # custom Hyprland build: noshare-cover.lib.mkNoshareCover pkgs config.programs.hyprland.package
      lib = { inherit mkNoshareCover; };

      # Home Manager: the plugin is always built against the Hyprland you actually run,
      # so the ABI matches whether Hyprland comes from nixpkgs or the hyprwm flake.
      #   imports = [ inputs.noshare-cover.homeManagerModules.default ];
      #   programs.noshare-cover.enable = true;
      homeManagerModules.default =
        {
          config,
          lib,
          pkgs,
          ...
        }@args:
        let
          cfg = config.programs.noshare-cover;
          hmHyprland = config.wayland.windowManager.hyprland.package;
          # When the NixOS module (programs.hyprland) is enabled, the session runs that
          # Hyprland through /run/wrappers, so it wins over the Home Manager one.
          osCfg = (args.osConfig or { }).programs.hyprland or { };
          osHyprland = if osCfg.enable or false then osCfg.package or null else null;
          hyprlandPkg =
            if cfg.hyprlandPackage != null then
              cfg.hyprlandPackage
            else if osHyprland != null then
              osHyprland
            else if hmHyprland != null then
              hmHyprland
            else
              pkgs.hyprland;
        in
        {
          options.programs.noshare-cover = {
            enable = lib.mkEnableOption "noshare-cover, built against the Hyprland in use";
            hyprlandPackage = lib.mkOption {
              type = lib.types.nullOr lib.types.package;
              default = null;
              description = "Hyprland to build against. Default: the system programs.hyprland.package when that module is enabled, then wayland.windowManager.hyprland.package, then pkgs.hyprland.";
            };
            package = lib.mkOption {
              type = lib.types.package;
              readOnly = true;
              default = mkNoshareCover pkgs hyprlandPkg;
              description = "The built plugin.";
            };
          };
          config = lib.mkIf cfg.enable {
            wayland.windowManager.hyprland.plugins = [ cfg.package ];
          };
        };

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.cargo
            pkgs.rustc
            pkgs.clippy
            pkgs.rustfmt
            pkgs.pkg-config
            pkgs.nasm
          ];
        };
      });
    };
}
