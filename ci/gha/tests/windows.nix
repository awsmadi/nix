{
  nixFlake ? builtins.getFlake ("git+file://" + toString ../../..),
  system ? builtins.currentSystem,
  pkgs ? nixFlake.inputs.nixpkgs.legacyPackages.${system},
}:

let
  packages = nixFlake.packages.${system};

  fixOutput =
    test:
    test.overrideAttrs (prev: {
      nativeBuildInputs = prev.nativeBuildInputs or [ ] ++ [ pkgs.colorized-logs ];
      env.GTEST_COLOR = "no";
      # Wine's console emulation wraps every character in ANSI cursor
      # hide/show sequences, making logs unreadable in GitHub Actions.
      buildCommand = ''
        set -o pipefail
        {
          ${prev.buildCommand}
        } 2>&1 | ansi2txt
      '';
    });
in

{
  unitTests = {
    "nix-util-tests" = fixOutput packages."nix-util-tests-x86_64-w64-mingw32".passthru.tests.run;
  };

  /*
    Compile every Windows component.

    `unitTests` above builds one suite, and `nix-util-tests` links neither
    libmain nor libstore, so a change can break the Windows build outright while
    this file stays green. That happened: a `Pid` comparison valid only on Unix
    left `nix-main-x86_64-w64-mingw32` failing to compile for days behind a green
    "windows unit tests" job.

    This needs no emulator and cannot be flaky -- it either compiles or it does
    not -- so it is the cheapest useful signal available for the target.
  */
  crossBuild = packages."nix-everything-x86_64-w64-mingw32";
}
