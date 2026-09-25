{
  description = "Lox++ interpreter and language server";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
  };

  outputs = { self, nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      buildDeps = with pkgs; [
        cmake
        ninja
        clang
        clang-tools
        lld
        pkg-config
        python3
        gtest
      ];
      buildFlags = [
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_C_COMPILER=clang"
        "-DCMAKE_CXX_COMPILER=clang++"
        "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
        "-DLOXPP_DEBUG_PRINT_CODE=OFF"
        "-DLOXPP_DEBUG_TRACE_EXECUTION=OFF"
        "-DLOXPP_DEBUG_LOG_GC=OFF"
        "-DLOXPP_BUILD_TESTS=OFF"
        "-G" "Ninja"
      ];
      mkLoxppDerivation = derivationArgs:
        pkgs.stdenv.mkDerivation ({
          name = derivationArgs.name;
          src = pkgs.cleanSource ./.;
          nativeBuildInputs = buildDeps;
          buildInputs = [ pkgs.gtest ] ++ (derivationArgs.extraBuildInputs or []);
          doCheck = derivationArgs.doCheck or false;
          cmakeFlags = buildFlags ++ (derivationArgs.extraCmakeFlags or []);
          postInstall = derivationArgs.postInstall or "";
          meta = derivationArgs.meta or {};
        });
    in {
      packages = {
        loxpp = mkLoxppDerivation {
          name = "loxpp";
          meta = {
            description = "Lox++ bytecode VM interpreter";
            mainProgram = "loxpp";
            homepage = "https://github.com/txloc1909/loxpp";
            license = pkgs.licenses.mit;
            platforms = [ "x86_64-linux" ];
          };
        };

        loxpp-lsp = mkLoxppDerivation {
          name = "loxpp-lsp";
          extraCmakeFlags = [
            "-DLOXPP_LSP=ON"
          ];
          meta = {
            description = "Lox++ language server (LSP)";
            mainProgram = "loxpp-lsp";
            homepage = "https://github.com/txloc1909/loxpp";
            license = pkgs.licenses.mit;
            platforms = [ "x86_64-linux" ];
          };
        };

        default = self.packages.loxpp;
      };

      devShells.default = pkgs.mkShell {
        buildInputs = buildDeps ++ [
          git
        ];
        shellHook = ''
          echo "Lox++ development environment"
          echo "Build with: cmake --preset release && cmake --build build"
        '';
      };
    };
}
