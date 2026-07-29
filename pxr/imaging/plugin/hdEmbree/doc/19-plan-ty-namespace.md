# Plan: Complete the `ty` namespace pass over `renderer/`

Status: implemented 2026-07-29. Repository-wide rename over `renderer/`, no
behavior change. Completes the namespace ownership invariant introduced by
plan 14.

Sequencing: depends on 14, 17, and 18. Must precede the cross-struct naming
sweep in 20 and the final contracts in 21.

## Final scope

The original draft also proposed removing every anonymous namespace, making
every translation-unit-contained function/object `static`, relocating
translation-unit-contained types, and changing MaterialXCpp linkage. That work
was withdrawn after implementing plan 14.

Plan 19 owns only:

- shared Typhoon renderer declarations and definitions moving into
  `PXR_NAMESPACE::ty`;
- renderer-owned types and free functions dropping redundant
  `HdEmbree`/`HdEmbree_` prefixes;
- explicit `ty::` qualification at delegate, test, and MaterialXCpp call sites;
- documentation of the resulting ownership boundary.

Existing translation-unit linkage remains unchanged. File-local implementation
stays at `PXR_NAMESPACE` scope as `static` declarations or in its existing
anonymous namespace. Existing anonymous namespaces are not a validation
failure.

## Target invariant

Shared Typhoon-owned declarations under `renderer/` live in
`PXR_NAMESPACE::ty`, use no leading underscore, and carry no redundant
`HdEmbree`/`HdEmbree_` prefix.

Exceptions:

| Area | Ownership |
| --- | --- |
| `renderer/materials/MaterialXCpp/` | Existing `mxcpp` ownership and linkage |
| `renderer/integrator/medium.*` | Existing pxr-independent `mxcpp` API |
| `renderer/materials/BSDL/` | Vendored; unchanged |
| `renderer/lights/pxrIES/` | Vendored; unchanged |
| `renderer/debugCodes.*` | `TF_DEBUG_CODES` stays directly in `PXR_NAMESPACE`; the macro specializes `TfDebug::_Traits` and cannot be nested in `ty` |
| `delegate/` | Hydra-facing declarations stay in `PXR_NAMESPACE` with their `HdEmbree` names |

`HDEMBREE_LIGHT_CREATE` remains runtime-visible under the same name.

`proceduralHelpers.h` was the one anonymous namespace in a header. It was
removed as a separate ODR/linkage correction: its functions and templates were
already `inline`, and its three constants became `inline constexpr`. This does
not reinstate the withdrawn tree-wide anonymous-namespace sweep.

## Ownership boundaries

Delegate names retain their Hydra-facing role:

- `HdEmbreeMesh`
- `HdEmbree_Light`
- `HdEmbreeRenderBuffer`
- `HdEmbreeInstancer`
- `HdEmbreeMaterial`
- `HdEmbreeRenderDelegate`
- `HdEmbreeRenderPass`
- `HdEmbreeRenderParam`

The registered types also remain unchanged:

| `plugInfo.json` | Source |
| --- | --- |
| `HdEmbreeRendererPlugin` | `delegate/rendererPlugin.*` |
| `HdEmbree_ImplicitSurfaceSceneIndexPlugin` | `delegate/implicitSurfaceSceneIndexPlugin.*` |

Moving either registered type would change its demangled `TfType` name and
silently break plugin discovery.

## Implemented type renames

Representative families:

| Previous | Current |
| --- | --- |
| `HdEmbreeRenderer` | `ty::Renderer` |
| `HdEmbreeRenderSettings` | `ty::RenderSettings` |
| `HdEmbreeRenderBufferInterface` | `ty::RenderBufferInterface` |
| `HdEmbreePrototypeContext` | `ty::PrototypeContext` |
| `HdEmbreeInstanceContext` | `ty::InstanceContext` |
| `HdEmbreePrimvarSampler` and subclasses | `ty::PrimvarSampler` and prefix-free subclasses |
| `HdEmbreeSampleDomain`, `HdEmbreeSampler` | `ty::SampleDomain`, `ty::Sampler` |
| `HdEmbreeLightSampler`, `HdEmbreeLightRegistry` | `ty::LightSampler`, `ty::LightRegistry` |
| `HdEmbree_LightData` | `ty::LightData` |
| `HdEmbree_Rect`, `HdEmbree_Sphere`, etc. | `ty::RectLight`, `ty::SphereLight`, etc. |
| `HdEmbreeMaterialData` | `ty::MaterialData` |
| `HdEmbreeMaterialEvalServices` | `ty::MaterialEvalServices` |
| `HdEmbreeOiioTextureSystem` | `ty::OiioTextureSystem` |
| `HdEmbreeRenderColorSpace` | `ty::RenderColorSpace` |
| `_HeroWavelengthState` | `ty::HeroWavelengthState` |

`mxcpp::MediumTransportModel`, `mxcpp::AdobeOpenPbrVolumeProperties`, and
`mxcpp::MediumProperties` deliberately remain in `mxcpp`. `medium.h` has no PXR
dependency and is part of the pxr-independent MaterialXCpp-facing API.

## Implemented non-type renames

- Prefixed geometry, displacement, wireframe, SSS, light, light-linking,
  sampling, render-setting, and color-management functions moved to `ty` and
  dropped `HdEmbree`.
- `HdEmbreeAovTokens` became `ty::AovTokens`.
- `HdEmbreePrimvarSamplingDetail` became `ty::PrimvarSamplingDetail`.
- Renderer defaults and policy constants in `renderSettings.h` moved to `ty`
  and dropped their prefix.
- `ConvertHdNetworkToMxcppGraph` moved to `ty`.
- `HDEMBREE_LIGHT_CREATE` did not move for the macro constraint above.

## Source-definition rule

Headers open `namespace ty` around shared renderer declarations. Source files
leave file-local implementation at its existing scope and explicitly qualify
shared definitions, for example:

```cpp
PXR_NAMESPACE_OPEN_SCOPE

static bool
_ValidateLocalInput(...)
{
    ...
}

void
ty::Renderer::Render(...)
{
    ...
}

PXR_NAMESPACE_CLOSE_SCOPE
```

`TF_DEFINE_PUBLIC_TOKENS` is the narrow macro exception: it is expanded inside
a small `namespace ty` block because the macro cannot accept a qualified token
key.

## Validation

These mechanical checks must return no stale renderer-owned declarations:

```sh
VENDORED=(-g '!renderer/materials/BSDL/**' -g '!renderer/lights/pxrIES/**')

rg -n '^\s*(struct|class|enum class|enum|using)\s+HdEmbree' renderer \
    "${VENDORED[@]}" -g '!renderer/materials/MaterialXCpp/**'
rg -n '\bHdEmbree[A-Za-z0-9_]*\s*\(' renderer \
    "${VENDORED[@]}" -g '!renderer/materials/MaterialXCpp/**'
rg -n 'namespace\s+HdEmbree|\bty::ty::' renderer delegate testenv \
    "${VENDORED[@]}"
```

Owner-namespace review checks every non-exempt renderer header for
`namespace ty`. Anonymous-namespace counts and TU-contained type uniqueness are
not completion criteria because their proposed migration was withdrawn.

Runtime validation:

- `pixi run build`;
- all focused hdEmbree CTests;
- standalone sampling and light-sampler tests;
- installed plugin, registered renderer/scene-index types, and
  `TyphoonRenderSettingsAPI` discovery;
- the complete fixed-reference Typhoon image suite.

## Completion record

- Build passed.
- Focused CTests passed: 6/6.
- Sampling tests passed: 7/7.
- Light-sampler tests passed: 24/24.
- Plugin and schema discovery passed.
- Namespace/prefix audits and `git diff --check` passed.
- Full Typhoon suite passed: 436/436 in 291.31 seconds. This exceeded the
  250-second advisory threshold but timing is non-failing.

No commit is created until Anders reviews and explicitly approves the completed
changes.
