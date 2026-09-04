# Building Typhoon for Houdini 22

Run every command in this document from the local Houdini workspace:

```bash
cd third_party/houdini/typhoon
```

## Compatibility and dependencies

The Houdini package target supports Linux x86-64, Windows x86-64, and Apple
Silicon macOS. Build natively on the target operating system; package binaries
are not portable between operating systems or Houdini builds.

The target compiles against the ABI-sensitive libraries supplied by Houdini
rather than the normal Pixi dependency set. It verifies OpenUSD 0.26.5
(`PXR_VERSION` 2605), Embree 3, and OpenImageIO 2.5 during configuration.
Houdini also supplies Hydra, Imath, TBB, C++20, and the platform C++ ABI. This
directory has its own `pixi.toml` and `pixi.lock`; that isolated Pixi workspace
supplies only CMake, Ninja, and OpenQMC 0.7.1.

Use a compiler compatible with the installed Houdini SDK: the matching GCC on
Linux, Visual Studio 2022 on Windows, or Xcode/Apple Clang on macOS. Houdini 22
for macOS supports Apple Silicon, so the Pixi platform is `osx-arm64`.

## Build and package

Set `HFS` to the Houdini installation used to build and run the package. On
Windows and macOS, launching the Houdini **Command Line Tools** supplies an
environment configured for the installed SDK. Otherwise set it explicitly.

Linux or macOS:

```bash
export HFS=/opt/sidefx/houdini/Houdini22.0.396
```

Windows PowerShell:

```powershell
$env:HFS = 'C:\Program Files\Side Effects Software\Houdini 22.0.396'
```

The four supported tasks are:

```bash
pixi run houdini-configure
pixi run houdini-build
pixi run houdini-package
pixi run houdini-smoke
```

Each task depends on the preceding task, so this single command performs the
complete configure, build, package, Embree 3 direct curve-intersection test,
renderer discovery, Render Settings LOP UI integration, and tube/ribbon
smoke-render workflow:

```bash
pixi run houdini-smoke
```

The source files and generated build tree are kept together under
`third_party/houdini/typhoon`:

- `doc/package-README.md.in`: template for the installed package `README.md`.
- `doc/licenses/`: third-party notices copied into the installed package.
- `soho/parameters/HdEmbreeRendererPlugin_Global.ds`: Typhoon controls added
  to standard Render Settings LOPs.
- `build/`: standalone CMake build tree.
- `build/testHdEmbreeCurveIntersections`: direct production-helper contract
  test (`.exe` on Windows).
- `build/package/`: contents ready to copy into Houdini's `packages/`
  directory. It contains `typhoon.json` and the `typhoon/` payload directory.
- `build/smoke.exr`: smoke-test output.

Render from the command line with the package wrapper:

Linux or macOS:

```bash
build/package/typhoon/husk-typhoon \
    -R HdEmbreeRendererPlugin \
    -o output.exr \
    scene.usda
```

Windows:

```powershell
build\package\typhoon\husk-typhoon.cmd `
    -R HdEmbreeRendererPlugin `
    -o output.exr `
    scene.usda
```

Install the package for the current user by copying the contents of the staging
directory directly into the `packages` directory below
`HOUDINI_USER_PREF_DIR`. Use `hconfig -ap HOUDINI_USER_PREF_DIR` from Houdini's
Command Line Tools to find the exact platform-specific directory.

For the default Linux location:

```bash
mkdir -p "$HOME/houdini22.0/packages"
cp -a build/package/. "$HOME/houdini22.0/packages/"
```

Do not copy only `hdEmbree.so`: the package also contains plugin metadata,
schemas, the OpenQMC runtime library, renderer menu metadata, and license
notices. The included `typhoon.json` enables the adjacent `typhoon/` payload.
The generated `typhoon/README.md` documents renderer selection in the Solaris
UI.

A package is valid only for the Houdini SDK against which it was configured.
Rebuild it for a different Houdini release. Do not add Pixi-provided OpenUSD,
Embree, OpenImageIO, Imath, TBB, or Python libraries to this package or its
runtime path.

The smoke task first requires the direct intersection test to report Embree 3
and pass its tube/ribbon endpoint, shading-frame, and ownership checks. It then
checks renderer discovery, verifies that the Render Settings LOP authors the
float `ty:minCurveWidth` property with default `0.001`, and renders the 64x64
smoke scene. Starting Houdini tools requires access to a valid license service.
A licensing failure after a successful build and direct test does not indicate
a compiler or plugin-loading failure; restore license access and rerun
`houdini-smoke`.
