# Plan: Delete the environment-variable config path and consolidate runtime render settings into one plain value

Status: structural cleanup **plus one intentional behavior removal**. This is no
longer behavior-preserving: every `HDEMBREE_*` render-config override formerly
owned by `HdEmbreeConfig` stops working. Settings come from schema attributes on
the render settings prim (or direct delegate overrides), falling back to
hard-coded defaults, and nowhere else.

This does not touch other `HDEMBREE_*` identifiers, which are unrelated: the
`HDEMBREE_LIGHT_CREATE` `TfDebug` code (debugCodes.h:16), the
`PXR_HDEMBREE_ENABLE_ADOBE_OPENPBR` compile definition, and the
`HDEMBREE_RENDER_SETTINGS_TOKENS` macro all survive.

Sequencing: depends on plans 06 and 11; precedes plans 14 and 16. Plan `01`
lands first
and makes every entry private, so the list this plan removes `renderer/config`
from is `PRIVATE_CLASSES`. Serialize with any
other work touching `renderer/renderer.h`, `renderer/renderer.cpp`,
`renderer/config.*`, `delegate/renderDelegate.cpp`, or
`delegate/renderPass.cpp`.

## Why the two changes are one plan

They interlock. Doing the consolidation first would mean writing a
`FromConfig()` factory and a two-source token-fallback policy specifically to
delete them a plan later. Doing the config removal first makes the
consolidation strictly simpler:

- `FromConfig()` never exists — the struct's default member initializers *are*
  the hard-coded defaults.
- The renderer constructor becomes a default-initialized `_settings`.
- Token validation collapses to one path. Today `renderPass.cpp:852-861`
  silently canonicalizes an invalid *config* sampler string before the delegate
  lookup, while an invalid *delegate* token warns. With no config strings there
  is only the delegate path, which warns.

So: **Part A deletes `HdEmbreeConfig`, then Part B consolidates.** Implement in
that order.

## Commit boundaries

Parts A and B are not one indivisible edit. Three commits each build and pass
tests on their own:

1. **Promote `tileSize` and `jitterCamera`.** Tokens, schema attributes,
   descriptors, renderLab metadata, regenerated schema. Still reads
   `HdEmbreeConfig`. Self-contained and independently reviewable.
2. **Delete `HdEmbreeConfig`.** Add the settings header with all defaults;
   repoint every consumer at struct defaults and named constants; delete
   config.h/.cpp; add the parity test.
3. **Consolidate.** Add `SetRenderSettings()`, convert renderer reads to
   `_settings.field`, delete the 24 setters and their members.

Commit 2 leaves a transient state where the renderer still has its ~25
standalone members, now initialized from `HdEmbreeRenderSettings{}` rather than
`config`. That duplication is real but trivial — one initializer list — and it
buys three reviewable commits instead of one large migration. Prefer it.

## Issue

A runtime setting is currently repeated across `HdEmbreeConfig` (a default
constant, a member, a doc comment, and an env-var parse), the delegate token and
descriptor, the schema attribute, the `_Execute()` lookup, a `SetX()`
declaration and definition, a renderer member, and direct reads in the renderer.

`renderer/config.cpp` is 312 lines of environment parsing for 30 settings.
`renderer.h:238-378` spends ~140 lines on 24 individual setter contracts.
`renderPass.cpp:802-944` is a 143-line sequence of nearly identical
`GetRenderSetting<T>(token, config.x)` followed by `SetX(value)`. The actual
control flow — version changed, stop rendering, resolve policy, resync
materials, restart — is buried.

The environment path is also the wrong mechanism: it makes renders depend on
invisible process state, gives every setting a second precedence rule nobody
audits, and duplicates defaults that the schema already declares.

## Goals

- Delete `HdEmbreeConfig` and all 30 `TF_DEFINE_ENV_SETTING` declarations.
- Establish one hard-coded default per render setting, and test it against the
  schema fallback.
- Represent all renderer-consumed runtime settings in one plain struct.
- Apply settings as one update while this renderer is stopped.
- Remove every render-setting-specific setter and its standalone member.
- Give the render pass and the renderer distinct, stated responsibilities.

## Non-goals

- No metadata registry, macro table, reflection system, or templated framework.
- Do not fold camera, AOV bindings, scene handles, frame/time, light
  registration, or wireframe state into user render settings.
- Do not change existing token names, default *values*, or authored precedence
  between the render settings prim and direct delegate overrides.
- Do not add validation or clamping to existing settings. (`tileSize` is newly
  exposed and does need an invariant — see Part A.)
- Do not otherwise change schema or descriptor definitions. Two attributes are
  added for `tileSize` and `jitterCamera`; no existing attribute changes. The
  parity test may reveal a mismatch — fixing one is in scope, redesigning the
  schema is not.
- Do not promote `cameraLightIntensity` (see Part A).
- Do not deglobalize the MaterialXCpp policy.

# Part A — Delete `HdEmbreeConfig`

## What goes

- `renderer/config.cpp` in full (312 lines): 30 `TF_DEFINE_ENV_SETTING`
  declarations, the parsing constructor, `HDEMBREE_PRINT_CONFIGURATION`
  reporting, and the singleton.
- `renderer/config.h` in full (248 lines), and the `renderer/config` entry in
  `PRIVATE_CLASSES` (`PUBLIC_CLASSES` until plan `01` lands).

No `TF_DEFINE_ENV_SETTING` or `TfGetEnvSetting` exists anywhere else in the
plugin, and no test in `testenv/` sets an `HDEMBREE_*` variable, so nothing else
needs migrating.

`config.h` is not an installed header once plan `01` has landed, which it does
before this plan. Even before that it had no consumer: nothing outside
`pxr/imaging/plugin/hdEmbree/` in this repository includes it or names
`HdEmbreeConfig`, and there are no Python bindings, so no Python consumer can
exist. Either way no deprecation decision is owed — `01` records the project
decision that hdEmbree exposes no supported hand-written C++ API.

## One render-settings boundary owns every C++ default

Put all of the following in a single new header, so there is exactly one place
to look for a default:

- `HdEmbreeRenderSettings` and its member-initializer defaults;
- named defaults for the settings deliberately *not* in the struct —
  `enableAmbientOcclusion` (literal `false`), `enableExposureCompensation`,
  `dynamicSubdvTesselation`, `materialRenderContext`;
- `HdEmbreeDielectricLayerThroughputMode` and its two token conversions;
- `HdEmbreeCameraLightIntensity`.

`domeLightCameraVisibility` keeps its struct default but is an explicit
exception on the schema side: it is a core `HdRenderSettingsTokens` concept with
no `ty:` attribute.

## The supported external surface is USD

State this in `ARCHITECTURE.md` as part of the change:

> hdEmbree's supported external render-settings interface is the USD
> `TyphoonRenderSettingsAPI` schema. Direct Hydra delegate settings remain an
> internal application-control path used by clients such as usdview's renderLab.
> `HdEmbreeConfig`, `HdEmbreeRenderer`, and `HdEmbreeRenderSettings` are
> implementation details, not a supported C++ API.

Consequences for this plan:

- No C++ deprecation period is owed for `HdEmbreeConfig`; its installation was
  accidental implementation exposure.
- **The new settings header goes in `PRIVATE_HEADERS`.** No question arises
  about the `renderDelegate.h` → `renderer.h` → settings-header include chain
  (`renderDelegate.h:302` holds `HdEmbreeRenderer _renderer` by value, so it
  needs the complete type): after plan `01` every entry is private, so the whole
  chain is private and self-consistent. Private headers are still copied into
  the build include tree, so nothing about compilation changes.

## Interactive settings are unaffected

`HdEmbreeConfig` is not a runtime control surface: `GetInstance()` returns a
`const` reference, the constructor is private, and it is populated once from the
environment at first use. Nothing can mutate it after startup.

Interactive settings changes — including usdview's renderLab plugin — go through
the standard Hydra path instead, which this plan preserves unchanged:

- `extras/usd/examples/usdviewPlugins/renderLab/renderSettingsEditor.py:119`
  calls `stageView.GetRendererSettingsList()`, which resolves to
  `HdRenderDelegate::GetRenderSettingDescriptors()` and hence to
  `_settingDescriptors` in renderDelegate.cpp.
- Lines 345 and 362 call `stageView.SetRendererSetting()`, which resolves to
  `HdRenderDelegate::SetRenderSetting()`, bumping the settings version so
  `_Execute()` picks the change up on the next frame.
- Line 372 calls `stageView.GetRendererSetting()` to read the current value.

The only coupling is each descriptor's default value, which renderLab shows as
the un-authored value and restores via its reset button
(`_originalValue`/`_resetSetting`, lines 354-381). That default moves from
`config.x` to the corresponding `HdEmbreeRenderSettings` field. Same API, same
values.

This is in fact an improvement for renderLab: today `HDEMBREE_MAX_BOUNCES=4`
makes the plugin display 4 as the setting's default and reset to 4. Afterwards
the displayed defaults are stable and match the schema, enforced by the parity
test below.

## Where the seven consumers go

| Consumer | Today | After |
| --- | --- | --- |
| `renderDelegate.cpp:135` descriptor list | `config.x` | corresponding field of a default-constructed `HdEmbreeRenderSettings` |
| `renderPass.cpp:802` fallbacks | `config.x` | same |
| `renderer.cpp:95-132` constructor | 30 `config.x` initializers | `_settings` default-initialized |
| `oiioTextureSystem.cpp:379` | `config.textureCacheSizeMB` | `HdEmbreeRenderSettings{}.textureCacheSizeMB` |
| `renderer.cpp` ×2 | `config.tileSize` | `HdEmbreeTileSize` constant |
| `camera.cpp` | `config.jitterCamera` | `HdEmbreeJitterCamera` constant |
| `unlitIntegrator.cpp` | `config.cameraLightIntensity` | `HdEmbreeCameraLightIntensity` constant |

## The three env-only knobs

`tileSize`, `jitterCamera`, and `cameraLightIntensity` have no token, no
descriptor, and no schema attribute today. They were reachable only by
environment variable, so deleting the env path would otherwise delete the
capability.

**Promote `tileSize` and `jitterCamera` to real render settings.** Both are
consumed inside `HdEmbreeRenderer` methods, so both become ordinary
`_settings` fields:

- `tileSize` — renderer.cpp:500 and :812-813, work partitioning for
  `_RenderTiles`.
- `jitterCamera` — camera.cpp:24, inside `_SampleCameraRay`.

Each needs the full four-surface treatment:

| Surface | `tileSize` | `jitterCamera` |
| --- | --- | --- |
| Token in `HDEMBREE_RENDER_SETTINGS_TOKENS` (renderDelegate.h:30-55) | `ty:tileSize` | `ty:jitterCamera` |
| Schema attribute (schema/schema.usda) | `uniform int ty:tileSize = 8` | `uniform bool ty:jitterCamera = true` |
| Descriptor entry (renderDelegate.cpp:136) | "Tile Size" | "Jitter Camera Rays" |
| `HdEmbreeRenderSettings` field | `int tileSize = 8` | `bool jitterCamera = true` |

Also add renderLab metadata entries in
`extras/usd/examples/usdviewPlugins/renderLab/renderSettingsMetadata.py`, whose
`"settings"` table assigns each key a display category. Suggested: `ty:tileSize`
→ `"Diagnostics"`, `ty:jitterCamera` → `"Sampling"`. Without entries they fall
into the default category.

`schema/generatedSchema.usda` and `generatedSchema.classes.txt` must be
regenerated with `usdGenSchema`. The schema sets `skipCodeGeneration = true`, so
no C++ is produced.

### `tileSize` needs a clamp, and that is not scope creep

`tileSize` is `unsigned` and used as
`(width + tileSize - 1) / tileSize` (renderer.cpp:501-504, 814-815). A value of
0 underflows to `UINT_MAX` and then divides by zero. Today that is
expert-only via an env var; exposing it in the UI makes it a reachable crash.

Clamp `std::max(1, tileSize)` in `SetRenderSettings()`, alongside the existing
`maxBounces` and `lightSamplesPerHit` clamps. This does not violate the "no new
validation" non-goal, which exists to stop scope creep on *existing* settings: a
newly exposed setting needs its invariant established at the same moment it
becomes reachable. No upper clamp is needed — a very large tile degenerates to a
single work item, which is slow but correct.

Field type is `int` in the struct, descriptor, and schema, matching how
`samplesToConvergence` and `textureCacheSizeMB` are already handled
(`unsigned`/`int` in config, `int` in the descriptor); the render loop does its
own unsigned conversion locally.

### `cameraLightIntensity` stays a constant — at 3.0f, not 300

`cameraLightIntensity` (unlitIntegrator.cpp:136) drives the fallback headlight,
which only applies when a scene has no lights.

**The constant is `3.0f`.** `HdEmbreeDefaultCameraLightIntensity` is 300, but
config.cpp:199-200 interprets it as a percentage:

```cpp
cameraLightIntensity = std::max(100, TfGetEnvSetting(...)) / 100.0f;
```

so the value actually consumed is `3.0f`. Freezing 300 would make fallback
headlight renders 100× too bright. Define:

```cpp
// Linear multiplier on the fallback headlight. Not a percentage.
constexpr float HdEmbreeCameraLightIntensity = 3.0f;
```

The `std::max(100, ...)` floor also meant the effective minimum was 1.0f; with
the env path gone there is nothing to clamp.

Promoting this to a render setting would need a considered story about how it
interacts with real scene lighting, which this plan does not have — a separate
plan if it is ever wanted. **This is now the only capability the env removal
drops.**

## Defaults needing attention

Making the hard-coded defaults the single source exposes four irregularities:

- **`samplerSequence` has no constant.** `config.samplerSequence` is populated
  at runtime from `HdEmbreeGetSamplerSequenceToken(HdEmbreeGetDefaultSamplerSequence())`
  (config.cpp:203-206). The struct field is typed
  `HdEmbreeSamplerSequence` and simply defaults to
  `HdEmbreeSamplerSequence::OpenQMCSobolBN` (sampling.h:62-65), whose token is
  `"openqmc_sobolbn"` — matching schema.usda:53.
- **`enableAmbientOcclusion`'s default is derived**
  (`HdEmbreeDefaultAmbientOcclusionSamples > 0`, config.h:22-23). Make it the
  literal `false`; the coupling is incidental and defeats a parity test.
- **`enableExposureCompensation` and `dynamicSubdvTesselation` have no
  constants at all** — `renderDelegate.cpp:161,216` hard-code `VtValue(true)`
  and `VtValue(false)` inline. Both are pass-owned rather than renderer-owned
  (see inventory), so give them named constants beside the settings struct so
  the parity test can reach them.
- **`materialRenderContext` also needs a named constant.** It has a descriptor
  and a schema attribute but is consumed via `GetMaterialRenderContexts()`, so
  it is not a `HdEmbreeRenderSettings` field and would otherwise lose its
  default when `HdEmbreeConfig` goes.
- **`domeLightCameraVisibility` is not a `ty:` attribute.** It uses
  `HdRenderSettingsTokens->domeLightCameraVisibility`, a core UsdRender concept,
  so it has a descriptor but no entry in `schema/schema.usda`. The parity test
  must exclude it explicitly rather than report it as missing.

## Consequences for Part B

- `FromConfig()` is never written.
- The renderer constructor is `HdEmbreeRenderer() = default;` with respect to
  settings.
- The sampler config-canonicalization block (renderPass.cpp:852-861) is deleted
  outright. Only the delegate token path survives, and it warns via round-trip
  validation.
- Dielectric token handling likewise reduces to one path: the delegate value,
  warning on an unknown token.

# Part B — Consolidate into one plain value

## The value

Add a plain `HdEmbreeRenderSettings` in renderer-owned code, grouped with
comments only: sampling/progressive; lighting/AO; adaptive sampling; path depth
and caustics; shadows; material/texture policy. **Every field carries its
hard-coded default as a member initializer.** This is the single source of truth
for defaults, tested against the schema.

Types are concrete — `HdEmbreeSamplerSequence`,
`HdEmbreeDielectricLayerThroughputMode`, and `textureCacheSizeMB` with the unit
in the name.

Add one renderer method:

```cpp
void SetRenderSettings(HdEmbreeRenderSettings const& settings);
```

## Responsibility split

> The render pass resolves Hydra values and cross-setting policy.
> `SetRenderSettings()` enforces renderer value invariants, applies side
> effects, and stores the normalized final value.

The render pass owns delegate lookups falling back to
`HdEmbreeRenderSettings{}`, token → typed-value conversion, warning on invalid
tokens, and cross-setting policy — notably lighting vs. AO:

```cpp
nextSettings.enableLighting = ...;

const bool enableAmbientOcclusion = ...;
nextSettings.ambientOcclusionSamples =
    !nextSettings.enableLighting && enableAmbientOcclusion
        ? renderDelegate->GetRenderSetting<int>(...)
        : 0;
```

`SetRenderSettings()` owns invariant enforcement before storing:

- `std::max(0, maxBounces)` — existing, renderer.cpp:193;
- `std::max(1, lightSamplesPerHit)` — existing, renderer.cpp:229;
- `std::max(1, tileSize)` — new, required because the setting is newly exposed
  and 0 is a divide-by-zero (see Part A).

That is the complete clamp inventory for user settings.
`SetWireframeStyle`'s `std::max(1.0f, lineWidth)` stays where it is — wireframe
is not a user setting.

## Token conversion

**Reuse the existing sampler helpers.** `renderer/sampling/sampling.h` already
provides `HdEmbreeGetSamplerSequenceFromToken()` (line 68, returns the default on
an unknown token) and `HdEmbreeGetSamplerSequenceToken()` (line 42, enabling
round-trip validation). Add nothing for sampler.

Add only `HdEmbreeDielectricLayerThroughputMode` and its two conversions,
mirroring that pattern. Name it in full to match the delegate token and the
MaterialXCpp concept while keeping ownership separate. Define it in hdEmbree
beside the settings struct; do **not** expose
`mxcpp::Bsdf::DielectricLayerThroughputMode` through renderer-facing settings or
make `renderer.h` include `renderer/materials/MaterialXCpp/materials/bsdf.h`,
which only `rendererImpl.h` pulls in today. Map hdEmbree → mxcpp inside
`SetRenderSettings()`.

Keep diagnostics out of the conversions: conversion returns the fallback, the
pass round-trips and warns, exactly as renderPass.cpp:866-875 does for sampler
today. Both settings now behave identically — unknown delegate token warns and
falls back to the canonical default. Making the dielectric field an enum deletes
`renderer.cpp:282-299` including its `TF_WARN`; that warning **moves** to the
pass, it does not disappear.

## Process-global settings

`_enableGgxMicrofacetMultipleScattering` (renderer.h:1276) and
`_dielectricLayerThroughputMode` (renderer.h:1279) are never read by any renderer
shading or tracing code — they are write-only mirrors of a process-wide
MaterialXCpp global that the constructor never pushes.

- **Delete both standalone members.** Their fields remain in `_settings` as part
  of the complete resolved value, but nothing reads them; `SetRenderSettings()`
  uses them solely to apply side effects. (`useAdobeOpenPBR` is different — real
  read sites, genuinely consumed.)
- **Apply both globals unconditionally on every `SetRenderSettings()` call**, as
  renderer.cpp:275-299 does today. A changed-value guard would compare against a
  local value never proven to match the global.

  Part A weakens the single-renderer version of this failure: with env overrides
  gone, the struct defaults and the mxcpp globals agree by construction (`true`
  and `Bsdl`; config.h:43,46 and bsdf.cpp:50-52), and the parity test keeps them
  agreeing. The failure now needs a delegate override that is set, applied,
  reverted to the default, and reapplied — or a second renderer in the process
  moving the global underneath the first. Both are reachable; unconditional
  application costs two calls and removes the class of bug entirely.
- **Document the pre-existing limitation:** these globals are process-wide.
  Stopping one renderer does not stop others, so changing them can affect
  another renderer shading concurrently. Per-evaluation state is separate work.

The texture-cache changed-value guard *is* safe: `HdEmbreeOiioTextureSystem`'s
constructor applies the same default (oiioTextureSystem.cpp:379), so a
default-initialized field accurately mirrors cache state from construction —
exactly the property the MaterialXCpp globals lack. It is also observationally
inert: `SetCacheSizeMB` (oiioTextureSystem.cpp:384-397) only writes
`max_memory_MB` on the two OIIO texture systems, so skipping an identical
assignment changes nothing.

Add no equality operator — only the texture-cache field needs comparison.

## First application

`_settings` is default-initialized at construction, and `FromConfig()` does not
exist, but the MaterialXCpp globals are still not pushed until the first apply.
Force it rather than relying on version numbers:

```cpp
if (!_hasAppliedRendererSettings ||
    _lastSettingsVersion != currentSettingsVersion) {
    ...
    _renderer->SetRenderSettings(nextSettings);
    _hasAppliedRendererSettings = true;
}
```

This fixes no current bug — `_lastSettingsVersion` inits to 0
(renderPass.cpp:342) and `HdRenderDelegate::_settingsVersion` to 1, so the first
`_Execute()` always applies. It pins an otherwise implicit cross-library
invariant.

## Material-context ordering

Preserve renderPass.cpp:792-961 exactly: detect changed material render
contexts; resolve and apply renderer settings while stopped; resync material
networks; re-read scene and displacement versions, because the resync can
publish a new displacement graph; restart with matching shading and tessellation
state.

# Settings inventory

Documentation only — not a runtime registry. Every entry in
`renderPass.cpp:802-944` appears exactly once. All defaults below become member
initializers on `HdEmbreeRenderSettings`.

## Values consumed during rendering

| Delegate key | Runtime field / type | Default | Normalization |
| --- | --- | --- | --- |
| `convergedSamplesPerPixel` | `samplesToConvergence` `int` | 256 | none |
| `enableLighting` | `enableLighting` `bool` | true | none |
| `domeLightCameraVisibility` (core token) | `domeLightCameraVisibility` `bool` | true | none |
| `enableSceneColors` | `enableSceneColors` `bool` | true | none |
| `randomNumberSeed` | `randomNumberSeed` `int` | -1 | none |
| `tileSize` *(new)* | `tileSize` `int` | 8 | `>= 1`, renderer-side |
| `jitterCamera` *(new)* | `jitterCamera` `bool` | true | none |
| `samplerSequence` | `samplerSequence` `HdEmbreeSamplerSequence` | `OpenQMCSobolBN` | token → enum, pass-side |
| `enableAdaptiveSampling` | `enableAdaptiveSampling` `bool` | true | none |
| `adaptiveThreshold` | `adaptiveThreshold` `float` | 0.01 | none |
| `minSamplesBeforeAdaptive` | `minSamplesBeforeAdaptive` `int` | 64 | none |
| `maxBounces` | `maxBounces` `int` | 16 | `>= 0`, renderer-side |
| `minBouncesBeforeRR` | `minBouncesBeforeRR` `int` | 2 | none |
| `lightSamplesPerHit` | `lightSamplesPerHit` `int` | 1 | `>= 1`, renderer-side |
| `stratifyLightSamples` | `stratifyLightSamples` `bool` | true | none |
| `showAdaptiveHeatmap` | `showAdaptiveHeatmap` `bool` | false | none |
| `fireflyClampThreshold` | `fireflyClampThreshold` `float` | 20.0 | none |
| `enableCaustics` | `enableCaustics` `bool` | false | none |
| `causticsClampThreshold` | `causticsClampThreshold` `float` | 5.0 | none |
| `approxTransparentShadows` | `approxTransparentShadows` `bool` | true | none |
| `disableShadows` | `disableShadows` `bool` | false | none |
| `useAdobeOpenPBR` | `useAdobeOpenPBR` `bool` | false | none |

`useAdobeOpenPBR` has six read sites (`integrator/unlitIntegrator.cpp:93`,
`integrator/visibility.cpp:49`, `integrator/lighting.cpp:339`,
`integrator/pathIntegrator.cpp:276`, `integrator/surfaceShading.cpp:679`,
`integrator/volumeTransport.cpp:89`) and is genuinely consumed.

## Derived runtime values

| Delegate keys | Runtime field | Policy |
| --- | --- | --- |
| `enableLighting`, `enableAmbientOcclusion` (default false), `ambientOcclusionSamples` (default 0) | `ambientOcclusionSamples` `int` | resolved pass-side; no `enableAmbientOcclusion` bool stored |

## Renderer-owned service settings

| Delegate key | Runtime field | Default | Side effect |
| --- | --- | --- | --- |
| `textureCacheSize` | `textureCacheSizeMB` `int` | 16384 | `HdEmbreeOiioTextureSystem::SetCacheSizeMB()` |

## Process-global side-effect settings

| Delegate key | Runtime field | Default | Side effect |
| --- | --- | --- | --- |
| `enableGgxMicrofacetMultipleScattering` | same, `bool` | true | `mxcpp::Bsdf::SetGgxMicrofacetMultipleScatteringEnabled()` |
| `dielectricLayerThroughputMode` | same, enum | `Bsdl` | `mxcpp::Bsdf::SetDielectricLayerThroughputMode()` |

## Pass-owned values, excluded from the renderer settings value

| Delegate key | Default | Notes |
| --- | --- | --- |
| `dynamicSubdvTesselation` | false | read outside the settings-version branch (renderPass.cpp:1026) |
| `enableExposureCompensation` | true | read outside the settings-version branch (renderPass.cpp:455) |
| `materialRenderContext` | `"mtlx"` | descriptor + schema, but consumed via `GetMaterialRenderContexts()` inside the settings-version branch (renderPass.cpp:792-800, 945-961); not a renderer setting |

Both `dynamicSubdvTesselation` and `enableExposureCompensation` still need named
default constants so the parity test can reach them.

## Frozen internal constants (not render settings)

`HdEmbreeCameraLightIntensity` = **3.0f** only — a linear multiplier, not the
percentage-style 300 that appears in `config.h`. No token, no descriptor, no
schema attribute.

# Exact removal and retention lists

Remove all 24 render-setting-specific setters (declarations and definitions),
together with every corresponding standalone member **that exists** —
`SetTextureCacheSize` has none, it forwards straight to the texture system:
`SetSamplesToConvergence`, `SetAmbientOcclusionSamples`,
`SetDomeLightCameraVisibility`, `SetEnableSceneColors`, `SetRandomNumberSeed`,
`SetEnableLighting`, `SetMaxBounces`, `SetMinBouncesBeforeRR`,
`SetSamplerSequence`, `SetEnableAdaptiveSampling`, `SetAdaptiveThreshold`,
`SetMinSamplesBeforeAdaptive`, `SetLightSamplesPerHit`,
`SetStratifyLightSamples`, `SetShowAdaptiveHeatmap`, `SetFireflyClampThreshold`,
`SetEnableCaustics`, `SetCausticsClampThreshold`, `SetApproxTransparentShadows`,
`SetDisableShadows`, `SetEnableGgxMicrofacetMultipleScattering`,
`SetDielectricLayerThroughputMode`, `SetUseAdobeOpenPBR`, `SetTextureCacheSize`.

No test in `testenv/` calls any of these directly, so full removal is achievable.

Retain (non-user-setting renderer state): `SetScene`, `SetDataWindow`,
`SetCamera`, `SetCameraExposureScale`, `SetCameraDepthOfField`,
`SetSceneFrameAndTime`, `SetAovBindings`, `SetWireframeStyle`, `AddLight`,
`RemoveLight`, `AddLightGeometry`, `RemoveLightGeometry`.

# Code-size gate

AGENTS.md:19 requires `(removed - added) >= 2 * added`, i.e.
`removed >= 3 * added`. The ratio is *not* removed/added.

**Estimated removal (~982):**

| Source | Lines |
| --- | --- |
| `renderer/config.cpp` in full | 312 |
| `renderer/config.h` in full | 248 |
| `renderer.h:238-378` setter declarations and doc comments (minus `SetWireframeStyle`) | ~133 |
| `renderer.cpp:151-323` setter definitions (minus `SetScene`, `SetWireframeStyle`) | ~165 |
| `renderer.cpp:95-132` constructor initializer entries | ~37 |
| `renderer.h:1226-1282` private setting members and comments (minus wireframe) | ~52 |
| `renderPass.cpp:802-944` net shrink, including the deleted sampler config canonicalization | ~35 |

**Estimated addition (~150):**

| Item | Lines |
| --- | --- |
| settings struct, ~25 fields with defaults and grouped comments | ~48 |
| `HdEmbreeDielectricLayerThroughputMode` enum and its two conversions | ~24 |
| `SetRenderSettings()` — clamps, globals, enum mapping, texture guard, store | ~32 |
| pass-owned and internal constants header | ~20 |
| pass-side dielectric round-trip validation and warning | ~10 |
| `_hasAppliedRendererSettings` member and guard | ~4 |
| `tileSize`/`jitterCamera` promotion — 2 tokens, 2 descriptors, 2 fields, 1 clamp | ~12 |

Net reduction ~832 against a required ~300 — clears the gate roughly
threefold. The previous version of this plan failed the gate at ~412 removed
against ~148 added; deleting `HdEmbreeConfig` is what makes it comfortably
viable, and dropping `FromConfig()` removes ~30 lines of the addition side.

Test code, schema files, and the renderLab Python metadata are excluded from
both columns: the rule governs the C++ abstraction, not its coverage or its
authored surfaces. The parity test is roughly 80 lines.

# Implementation sequence

**Part A first.** Writing `FromConfig()` and then deleting it is the main thing
this ordering avoids.

1. Add `HdEmbreeRenderSettings` with hard-coded default member initializers, the
   dielectric enum and conversions, and named constants for the pass-owned and
   internal values. Reuse the existing sampler conversions unchanged.
2. Promote `tileSize` and `jitterCamera`: add tokens, schema attributes,
   descriptor entries, and struct fields; regenerate the schema with
   `usdGenSchema`; add renderLab metadata entries.
3. Repoint all seven `HdEmbreeConfig` consumers at the struct defaults and the
   `cameraLightIntensity` constant.
4. Delete `renderer/config.h` and `renderer/config.cpp`; drop the
   `renderer/config` entry from `PRIVATE_CLASSES`.
5. Write the schema/default parity test and make it pass. Fix any mismatch it
   finds by correcting the hard-coded default to match the schema — the schema
   is the authored contract.
5. Add `SetRenderSettings()` — two clamps, unconditional globals with
   hdEmbree→mxcpp mapping, changed-value texture-cache resizing, then store.
6. Populate `nextSettings` in `_Execute()` with existing precedence, round-trip
   token validation, and lighting/AO policy; add `_hasAppliedRendererSettings`.
7. Convert renderer reads to `_settings.field` subsystem by subsystem.
8. Delete the setters, definitions, and members listed above.
9. Implement the remaining tests below.
10. Launch an adversarial review agent over the new tests, per the project's
    testing rule, and address or discuss its findings before landing.
11. Re-measure against `removed - added >= 2 * added`.
12. Update the documentation, including the other plans. Leaving these stale
    would have later plans instruct readers to use a removed mechanism.

    - `README.md`, `ARCHITECTURE.md`, `AGENTS.md`, `OPTIMIZATION.md` — ~40
      `HDEMBREE_*` references. `OPTIMIZATION.md` uses env vars as the
      *mechanism* for its tuning advice, so its recipes need rewriting against
      the render settings prim, not a find-and-replace.
    - `doc/README.md:9` — states the project-wide validation convention
      (`usdrender -s "{settings}.ty:randomNumberSeed = 1"` plus
      `oiiotool --diff`). This is the highest-leverage fix: every plan
      inherits it.
    - `doc/05-plan-naming-core.md`, `doc/06-plan-aov-dispatch.md`,
      `doc/07-plan-closure-classification.md` — fixed-seed and
      heatmap recipes.

    Replace fixed-seed recipes with
    `usdrender -s "{settings}.ty:randomNumberSeed = 1"`. Where a fixed scene
    frame already yields deterministic sampling (`randomNumberSeed = -1` uses
    the scene frame), say so instead of prescribing a seed.
13. Confirm the installed tree: clean configure, build, and install into a
    **fresh prefix** (a normal install does not remove headers an older build
    wrote); launch installed `usdview` and `usdrender` and confirm plugin and
    schema discovery.

# Validation

## Schema/default parity test

The point of the change: one default per setting, provably matching the schema.

Read fallbacks from the registered schema rather than parsing
`schema/schema.usda` as text — use
`UsdSchemaRegistry::GetInstance().FindAppliedAPIPrimDefinition("TyphoonRenderSettingsAPI")`
and `GetAttributeFallbackValue()`. Assert **bidirectionally**, so drift is caught
from either side:

- every `ty:` schema attribute has a corresponding hard-coded default of equal
  value;
- every entry in `_settingDescriptors` has a corresponding schema attribute,
  except `domeLightCameraVisibility`, which is a core `HdRenderSettingsTokens`
  concept with no `ty:` attribute and must be excluded by name;
- token-valued attributes' defaults are members of their `allowedTokens` list
  (`samplerSequence`, `materialRenderContext`,
  `dielectricLayerThroughputMode`).

Cover the named non-struct defaults too — `enableAmbientOcclusion`,
`enableExposureCompensation`, `dynamicSubdvTesselation`,
`materialRenderContext` — not only `HdEmbreeRenderSettings` fields.

**Normalize token representations before comparing.** Literal type equality is
impossible for the three token settings, which are deliberately spelled
differently at each layer:

| Setting | Descriptor | Schema | Runtime |
| --- | --- | --- | --- |
| `samplerSequence` | `std::string` | `token` | `HdEmbreeSamplerSequence` |
| `dielectricLayerThroughputMode` | `std::string` | `token` | enum |
| `materialRenderContext` | `std::string` | `token` | `std::string` |

Descriptors hold strings because `_GetTokenRenderSetting`
(renderPass.cpp:44-49) accepts either form. Do **not** change descriptor types
just to simplify the test — normalize all three sides to a canonical `TfToken`
and compare that.

Bool, int, and float settings keep exact type *and* value comparison. Exact
float comparison is right because both sides originate from literals; a
tolerance would hide precisely the drift being tested for.

## Env removal

- No `TF_DEFINE_ENV_SETTING` or `TfGetEnvSetting` remains in the plugin. Grep
  for those two, and for the specific removed variable names — **not** for
  `HDEMBREE_`, which still legitimately matches the `TfDebug` code, the compile
  definition, and the tokens macro.
- A representative default render with a removed variable set (e.g.
  `HDEMBREE_MAX_BOUNCES=1`) is bit-identical to one without it. This is the
  assertion that the override path is genuinely gone rather than merely unused.

## Load-bearing checks

- default-constructed `HdEmbreeRenderSettings` matches the descriptor defaults;
- the interactive override path still round-trips: for a representative
  setting, `SetRenderSetting()` changes the value reported by
  `GetRenderSetting()`, bumps the settings version, and reaches the renderer on
  the next `_Execute()`. This is the path renderLab drives;
- settings applied on the first `_Execute()` before any render;
- lighting/AO policy across all combinations of `enableLighting`,
  `enableAmbientOcclusion`, and `ambientOcclusionSamples`;
- unknown delegate token warns and falls back, for both `samplerSequence` and
  `dielectricLayerThroughputMode`. There is now only one fallback path per
  setting, so no subprocess or environment seam is needed;
- `maxBounces >= 0` and `lightSamplesPerHit >= 1` preserved, and the new
  `tileSize >= 1` clamp holds. Test `tileSize = 0` explicitly through the
  delegate: unclamped it is an unsigned underflow and a division by zero
  (renderer.cpp:501-504), so this is a crash test, not a formality;
- **`tileSize` does not affect the image.** Fixed-seed renders at tile sizes 8
  and 32 are bit-identical. Sample seeds derive from pixel coordinates, so tile
  partitioning is purely a work-distribution concern; this check would catch a
  tile-boundary regression that no other test targets;
- `jitterCamera = false` produces a stable, non-jittered image distinct from the
  default, confirming the newly exposed setting is actually wired through;
- **globals reapplied unconditionally** — a single-call test would not catch a
  bad changed-value guard, so drive the failure mode: apply settings, mutate the
  mxcpp global externally to the opposite value, reapply the *same*
  `HdEmbreeRenderSettings`, and verify the intended global is restored. Read back
  via `mxcpp::Bsdf::IsGgxMicrofacetMultipleScatteringEnabled()` (bsdf.h:24) and
  `GetDielectricLayerThroughputMode()` (bsdf.h:35);
- fixed-seed renders for default settings and representative authored overrides
  remain bit-identical to today's default-environment renders.

Existing render-settings descriptor, namespace, bridge-ownership, reset, and
auto-apply tests must pass unchanged, extended only to account for the two added
attributes. No existing attribute changes.

# Risks and decisions

- `tileSize` and `jitterCamera` gain a UI surface they never had. `tileSize` in
  particular is a performance knob users can now set badly; the clamp stops the
  crash but not a poor choice. The image-invariance test above is what keeps it
  honest as a pure performance control.
- `cameraLightIntensity` loses its only override mechanism and becomes a
  compile-time constant. Accepted; promoting it is a separate plan.
- Anyone relying on `HDEMBREE_*` overrides — local debugging habits, CI
  harnesses outside this repo, downstream integrations — breaks with no
  deprecation period. Nothing in this repo does.
- Deleting `renderer/config` removes no installed header: plan `01` lands first
  and privatizes every entry. Nothing in this repo includes it, and interactive
  clients use the Hydra render-settings API rather than the config singleton
  (see "Interactive settings are unaffected").
- `HDEMBREE_PRINT_CONFIGURATION` and its ~50-line reporting block go with the
  rest. No replacement is planned; the resolved settings are inspectable through
  the render settings prim.
- `enableAmbientOcclusion` stays a delegate-only input resolving into
  `ambientOcclusionSamples`; no redundant boolean is stored.
- Accumulation reset needs no attention: every `StartRender()` resets it via
  `_RenderCallback` (renderDelegate.cpp:114), so the settings path correctly has
  no `ResetAccumulation()` call today and must not gain one.

# Completion criteria

- `HdEmbreeConfig` no longer exists; no environment variable affects any render
  setting.
- Interactive settings still work end to end: usdview's renderLab lists every
  setting, changes take effect on the next frame, and reset restores the
  schema-matched default.
- One hard-coded default per setting, asserted equal to the schema fallback in
  both directions by a passing test.
- `ty:tileSize` and `ty:jitterCamera` exist as tokens, schema attributes,
  descriptors, struct fields, and renderLab metadata entries, with the schema
  regenerated.
- `_Execute()` presents one readable settings-resolution block and one apply
  call.
- All 24 listed setters are gone, with every standalone member that existed.
- Units, invariants, normalization, and token fallback documented once on the
  runtime value. Every setting has one C++ default definition at the private
  render-settings boundary and one USD schema fallback where applicable, bound
  together by the parity test.
- `ARCHITECTURE.md` states that the USD `TyphoonRenderSettingsAPI` schema is the
  supported external interface.
- No plan or document instructs the reader to use a removed environment
  variable.
- No new sampler conversion helper; `renderer.h` gains no MaterialXCpp include.
- Renders are unchanged for a default environment; only `HDEMBREE_*`-overridden
  environments differ.
- Measured reduction satisfies `removed - added >= 2 * added`.

## Mandatory final suite gate

After every plan-specific validation above, run the complete Typhoon suite as
the final gate:

```sh
cd ~/code/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

All tests must pass. The expected baseline is approximately 235 seconds. If any
test fails or runtime is 250 seconds or above, stop: do not continue or land
the plan. Check with Anders before proceeding.
