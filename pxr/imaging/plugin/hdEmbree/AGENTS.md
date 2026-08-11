# hdEmbree Agent Guide

This is the mandatory editing and validation guide for the `hdEmbree` Hydra
render delegate, also known as Typhoon. It is authoritative for coding rules,
build/test/profile commands, and maintenance pitfalls. Developer design belongs
in `ARCHITECTURE.md`; user-visible behavior belongs in `README.md`.

## Documentation map

- [`README.md`](README.md): user-facing capabilities, workflows, settings,
  AOVs, limitations, and examples.
- [`ARCHITECTURE.md`](ARCHITECTURE.md): authoritative dependency boundaries,
  ownership, frame/path flows, invariants, responsibility map, and extension
  points.
- `AGENTS.md`: mandatory coding rules and contributor workflow.
- [`overview.dox`](overview.dox): short generated Hydra/plugin overview and
  external runtime contract.
- [`OPTIMIZATION.md`](OPTIMIZATION.md): measured performance history and
  reproducible profiling findings.

Update every affected authority in the same change. A user-visible change
updates `README.md`; a design, ownership, invariant, or extension-point change
updates `ARCHITECTURE.md`; a contributor command or mandatory editing rule
change updates `AGENTS.md`; an external plugin identity, schema identity,
settings namespace, or AOV contract change updates `overview.dox`. Update more
than one only when the change crosses those boundaries. Keep optimization
measurements in `OPTIMIZATION.md`; include only
enough architectural context there to keep an entry durable.

Before editing renderer behavior, use the targeted links in
[Architecture quick links](#architecture-quick-links). Do not copy the linked
design back into this guide.

Unless a command block says otherwise, run commands from the OpenUSD
repository root.

## Project goals

Typhoon is a reference path tracer. Human and agent readability takes priority.

1. Keep it simple. Do not add an abstraction unless it removes at least twice
   as much code as it adds. Avoid verbose C++ constructs.
2. Prefer linear flow. Do not split logic into many tiny helpers that force the
   reader to jump around.
3. Comment intent, invariants, non-obvious policy, and each logical section's
   purpose and reason. Do not comment syntax or obvious plumbing. Function
   declarations document input invariants, failure modes, and returned errors.
   Do not add or rewrite comments in generated or vendored code solely to
   satisfy these rules.

## Naming rules

The complete, authoritative naming table is
[Common-quantity naming](ARCHITECTURE.md#common-quantity-naming). The condensed
rules below are mandatory but non-exhaustive. Read the complete table before
introducing or renaming a shared quantity:

- Use the established `normal`, `omegaIn`/`omegaOut`, `radiance`,
  `iorIn`/`iorOut`, `eta`, `absorption`, `scattering`, and `extinction` roots.
  IOR sides use the optics incident/transmitted convention; `eta` means only
  `iorIn / iorOut`.
- Use `pos` for positions, `dir` for directions, and `diffRay` for
  `RayDifferential`.
- Bare `u`/`v` are allowed for texture or surface-parametric coordinates;
  `x`/`y` for pixel column/row; `c` for a clear pixel/RGB channel index; and
  `bary` for barycentric components.
- Output pointer/reference parameters use `outFoo`. An `Out` within `foo`
  retains its exitant transport-side meaning.
- Bare `t` is allowed for a local ray/interpolation parameter. `i`/`j`/`k` are
  allowed only as positional loop indices. `idx` is allowed for an obvious
  local index when a more specific name adds nothing.
- A single-letter local may abbreviate a clearly named value in a short
  function when immediately consumed. `F0`, microfacet `D`/`G`, conductor
  `n`/`k`, and mix operands `fg`/`bg` retain their standard meanings.
- Derivatives use `d<Quantity>d<Variable>`, for example `dPdu`, `dPdv`,
  `dPdx`, and `dPdy`.

## Type rules

Name variable types explicitly whenever the exact type is reasonably short and
meaningful. `auto` is allowed only for:

- a range-for loop variable;
- an unwieldy standard-library iterator returned by `.find()` or a similar
  lookup;
- a `std::visit` visitor lambda parameter, with one visitor lambda per dispatch
  site and a comment identifying the C++17 exception;
- a generic lambda parameter for a local callable helper when every call site
  is in the same function, a concrete callable parameter would add runtime
  dispatch, and a comment identifies the performance exception;
- binding a lambda closure object;
- a structured binding.

This does not permit `auto` for a lookup result with a short meaningful type,
locals inside a visitor, or a generic lambda unrelated to `std::visit` or the
documented local callable-helper exception. When replacing `auto`, preserve
the deduced type exactly, including reference category, top-level constness,
and pointee constness. Do not substitute a convertible or base type.

## Source and linkage rules

- Dependencies flow `Hydra -> delegate/ -> renderer/`. Renderer code must not
  depend on Hydra-facing delegate implementation details.
- The source root is a private build include directory. Cross-directory
  first-party includes use angle brackets and start with `delegate/` or
  `renderer/`; same-directory includes use quotes and the basename. Never use
  a repository-wide prefix or `../`.
- Include groups run nearest to furthest: the translation unit's own header,
  same-directory headers, cross-directory hdEmbree headers, OpenUSD, third
  party, then C/C++ standard library. Separate groups by one blank line and
  sort within them. Preserve macro-sensitive/conditional sequences.
- Do not style-churn vendored `renderer/materials/BSDL/`.
- No hand-written hdEmbree header is installed or supported as a C++ API.
- Shared renderer declarations live in `PXR_NAMESPACE::ty`, have no leading
  underscore, and omit redundant `HdEmbree` prefixes.
- Translation-unit-only renderer functions/constants do not live in `ty`.
  Use `static _Foo` at `PXR_NAMESPACE` scope or preserve an existing anonymous
  namespace. Do not churn between those forms.
- `renderer/materials/MaterialXCpp/` and the pxr-independent
  `renderer/integrator/medium.*` retain their existing `mxcpp` and anonymous
  namespace conventions. Never introduce `mxcpp::ty` or an anonymous namespace
  in a header.
- `TF_DEBUG_CODES` stays directly at `PXR_NAMESPACE` scope.
  `HDEMBREE_LIGHT_CREATE` keeps its runtime-visible name.
- Renderer source definitions explicitly qualify shared `ty::` declarations.

## Build and run

The root `pixi.toml` owns configuration, installation, and environment
consistency:

- Configure: `pixi run configure`
- Build/install Release: `pixi run build`
- Configure profiling tree: `pixi run configure-profile`
- Build/install profiling plugin: `pixi run build-profile`
- Launch viewer: `pixi run usdview <stage.usda>`
- Render authored products: `pixi run usdrender <stage.usda>`
- Render under another root:
  `pixi run usdrender --outputRoot <dir> <stage.usda>`
- Legacy frame-recorder path:
  `pixi run usdrecord <stage.usda> <output-image>`

Always use `pixi run build` for normal builds and `pixi run build-profile` for
profiling builds. Never invoke Ninja or `cmake --build` directly, including
through `pixi run`. The Linux configure task uses Ninja, Release, Embree,
OpenImageIO, OpenQMC, and MaterialX and installs into `$CONDA_PREFIX`. The
Windows task uses the same core options, disables precompiled headers and CMake
regeneration, and adds `/utf-8`.

Pixi pins OpenQMC 0.7.1. The upstream `build_usd.py --embree` workflow also
downloads OpenQMC 0.7.1, and the hdEmbree CMake target requires the imported
`OpenQMC::OpenQMC` target. Keep these three provisioning surfaces synchronized
when changing the dependency.

The profiling build replaces the installed hdEmbree plugin. Run
`pixi run build` after profiling to restore the Release plugin. Profiling
configuration, rationale, measurements, and history belong in
`OPTIMIZATION.md`.

## Focused tests

Build before testing, then run relevant checks through Pixi:

- `pixi run ctest --test-dir build -R testHdEmbreeRenderSetup --output-on-failure`
- `pixi run ctest --test-dir build -R testHdEmbreeRenderSettings --output-on-failure`
- `pixi run ctest --test-dir build -R testHdEmbreeSampling --output-on-failure`
- `pixi run ctest --test-dir build -R testHdEmbreeWireframe --output-on-failure`
- `pixi run ctest --test-dir build -R testHdEmbreeLightSamplers --output-on-failure`
- `pixi run ctest --test-dir build -R testHdEmbreeSubdivision --output-on-failure`
- `pixi run build/pxr/imaging/plugin/hdEmbree/testMaterialXCpp`

Use broader CTest filters for shared renderer, material, sampling, or USD
imaging changes. Prefer CTest through Pixi when a test needs installed plugins
or resources.

For displacement, collect the AOUSD fixture with:

```sh
cd /path/to/aousd-materials-test-suite
pixi run pytest test-suite/surfaces/open_pbr_surface/displacement.usda \
    --collect-only -q
```

Render it with this checkout's installed `usdrender`; the suite environment may
resolve a separately packaged renderer.

The external `/path/to/typhoon-test-suite` repository owns rendered
regressions. Initialize its shaderball dependency with
`git submodule update --init --depth 1`. `materials/` covers renderer-focused
material/transport/geometry/texture cases; `test-suite/` covers broader
integration; `usdlux/` covers lights and LightAPI. Broad node-value coverage
stays in `testMaterialXCpp`.

The mandatory complete gate is:

```sh
cd /path/to/typhoon-test-suite
powerprofilesctl launch --profile performance -- \
    pixi run pytest --renderer typhoon-local
```

Let it finish. Every test must pass and elapsed time must be reported. For a
performance-sensitive change, compare the same workload before and after on
the same machine and investigate regressions. Do not commit or land until your
human reviews the completed changes and explicitly approves committing.

## Profiling

Build and confirm debug information:

```sh
pixi run configure-profile
pixi run build-profile
readelf -S .pixi/envs/default/plugin/usd/hdEmbree.so \
    | rg 'debug_info|debug_line'
```

Later builds may use `pixi run --skip-deps build-profile`. If `perf` reports
permission errors, check `kernel.perf_event_paranoid`.

Get the exact renderer command for one case from the owning rendered-regression
harness without rendering. Do not record a machine-specific checkout or
provider path. Profile that `usdrender` command, not pytest/report generation.
Fix `ty:randomNumberSeed` and keep all other scene settings unchanged. Run from the
OpenUSD repository root so `$PWD/.pixi` selects this provider.

Use at least five repetitions:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH="$PWD/.pixi/envs/default/bin:/usr/bin:/bin" \
    /usr/bin/perf stat -r 5 -d -o /tmp/hdembree-stat.txt -- \
    usdrender --complexity high --renderer Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
    <stage.usda> --outputRoot /tmp/hdembree-profile-stat
```

Collect renderer and worker stacks:

```sh
pixi run --clean-env -x /usr/bin/env \
    PATH="$PWD/.pixi/envs/default/bin:/usr/bin:/bin" \
    /usr/bin/perf record -o /tmp/hdembree.data \
    -F 499 -e cycles:u -g --call-graph fp -- \
    usdrender --complexity high --renderer Embree \
    -s "{settings}.ty:randomNumberSeed = 1" \
    <stage.usda> --outputRoot /tmp/hdembree-profile-record

perf report -i /tmp/hdembree.data
```

Inspect self cost and inclusive/children cost; on hybrid Intel CPUs inspect
`cpu_core` and `cpu_atom` separately. Re-record after every rebuild so build IDs
match. OpenUSD tracing complements statistical profiling:

```sh
PXR_ENABLE_GLOBAL_TRACE=1 \
pixi run usdrender \
    -s "{settings}.ty:randomNumberSeed = 1" \
    <stage.usda> --outputRoot /tmp/hdembree-profile-trace \
    > /tmp/hdembree-trace.txt 2>&1
```

Keep trace scopes out of per-ray/hit/BSDF loops. Compare renderer time, samples
per second, and end-to-end time. Validate fidelity and use material-heavy,
texture-heavy, many-light, and deep-path workloads before generalizing. Record
durable measurements in `OPTIMIZATION.md`; `/tmp` artifacts are ephemeral.

## Architecture quick links

- [Design boundary and external contract](ARCHITECTURE.md#design-boundary)
- [Repository responsibility map](ARCHITECTURE.md#repository-map)
- [Scene synchronization and geometry ownership](ARCHITECTURE.md#scene-synchronization)
- [Material compilation and evaluation](ARCHITECTURE.md#material-compilation-and-evaluation)
- [Light and shadow linking](ARCHITECTURE.md#light-and-shadow-linking)
- [Frame execution](ARCHITECTURE.md#frame-execution)
- [Pixel-to-path flow](ARCHITECTURE.md#how-a-pixel-sample-becomes-a-path)
- [Accumulation and AOV output](ARCHITECTURE.md#accumulation-and-aov-output)
- [Renderer reading order](ARCHITECTURE.md#reading-the-renderer)
- [Materials and MaterialX extension points](ARCHITECTURE.md#materials-and-materialx-nodes)
- [Lights](ARCHITECTURE.md#lights)
- [Sampling](ARCHITECTURE.md#sampling)
- [Geometry and primvars](ARCHITECTURE.md#geometry-and-primvars)
- [AOVs](ARCHITECTURE.md#aovs)
- [Render settings](ARCHITECTURE.md#render-settings)

## Safety invariants and editing pitfalls

These are concise pre-edit guards. Follow the architecture links for their full
design and owners.

- Stop rendering before mutating renderer-readable state.
  `HdEmbreeRenderParam::NotifySceneChange()` is the usual Sync path.
- Sync methods may run concurrently. Pull only data whose dirty bit is set.
- Embree geometry user data must outlive geometry visible to in-flight rays;
  release contexts and geometry in `Finalize()`.
- Do not assume scene-index and legacy `HdSceneDelegate` paths carry identical
  data. Check `sceneIndexAdapterSceneDelegate.cpp` when values are absent.
- Render setting defaults, schema types, UI values, and
  `GetRenderSetting<T>()` calls must agree. Preserve bridge-owned tracking so
  USD settings do not overwrite direct UI/delegate edits.
- `renderingColorSpace` is a standard `RenderSettings` attribute;
  `domeLightCameraVisibility` and `enableExposureCompensation` are generic
  Hydra settings. All three are unnamespaced. Never re-add the old
  `ty:domeLightCameraVisibility` or `ty:enableExposureCompensation` aliases.
- Keep `schema/schema.usda`, `schema/generatedSchema.usda`, descriptors, and
  tests synchronized for `TyphoonRenderSettingsAPI` changes. Edit RenderLab
  source under `extras/usd/examples/usdviewPlugins/renderLab/`, never the
  installed `.pixi` copy.
- Product output requires a valid, converged frame. Failed setup parks usable
  buffers but must not write a stale product.
- The renderer binding generation identifies the current live render pass.
  Only a pass whose installed generation remains current may report convergence
  or valid products; it reads its local explicit/anonymous buffers. Pass
  activation republishes pass-owned renderer state and restores its frozen
  adaptive-subdivision snapshot. Destroying the current owner clears its
  borrowed bindings, while destroying a non-current pass must not stop the
  active render.
- `usdrender` owns output-root and frame-placeholder expansion. hdEmbree
  receives the resolved `productName` and must not expand it again.
- Never add hit-time string or `TfToken` geomprop lookup. Materials compile
  constant names to handles; invalid names use handle `-1` and the authored
  default.
- Sample-domain keys are deterministic rendering state. Never reuse or
  renumber them or draw opportunistically from another domain.
- Geometric normals alone own topology, medium transitions, and ray offsets.
  Never infer them from smooth/displaced/material shading normals.
- Preserve indexed face-varying subdivision data and 16-byte Embree attribute
  padding. Different primvars may have different seams.
- Adaptive subdivision always starts from a fresh camera baseline. Never
  multiply cached levels. Preserve shared-edge consolidation, displaced-quad
  strip propagation, and 2:1 balancing before committing levels.
- Instance primvars cannot vary prototype displacement: Embree tessellates the
  shared prototype before applying instance transforms.
- Material and texture backend failures use documented fallbacks and must not
  escape Embree callbacks. OIIO owns the MaterialX-to-image V flip; do not add
  a graph-side or glTF-specific flip.
- Process-wide GGX/layer policy and the shared OIIO cache can affect another
  concurrently shading renderer. Do not treat them as per-renderer state.
