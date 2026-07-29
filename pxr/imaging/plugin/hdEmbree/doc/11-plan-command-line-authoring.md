# Plan: Author stage overrides from the `usdrender` command line

Status: **new capability** in `pxr/usdImaging/bin/usdrender/`. Nothing in
hdEmbree changes. Adds a repeatable `-s` / `--set` option that authors attribute
overrides into `usdrender`'s session layer:

```sh
usdrender scene.usda -r Embree \
    -s "{settings}.ty:maxBounces = 12" \
    -s "{settings}.ty:enableAdaptiveSampling = false" \
    -s "/World/Camera.focalLength = 35"
```

Sequencing: **must land before plan 12.** Plan 12 deletes every `HDEMBREE_*`
render-config environment variable, which is today the only way to override a
render setting without editing the stage. Landing 11 first means there is no
window in which command-line overrides are impossible; landing it after means
every profiling recipe, test harness, and agent workflow that depends on
`HDEMBREE_RANDOM_NUMBER_SEED=1` is broken in the interval.

No code dependency on any other plan; it is the sole editor of
`pxr/usdImaging/bin/usdrender/`. It **does** overlap plan 12 on documentation:
`AGENTS.md`, `README.md`, `OPTIMIZATION.md`, and `doc/README.md` all describe the
`HDEMBREE_*` mechanism and are edited by both. Since 11 lands first, it converts
those recipes to `--set` and 12 rebases onto the result, deleting the environment
path from prose that no longer depends on it.

## Issue

Plan 12's argument against the environment path is sound — invisible process
state, a second precedence rule nobody audits, defaults duplicated from the
schema — but it removes a capability without replacing it. What is actually
being used is not "environment variables"; it is **override a render setting for
one invocation without mutating the stage**. That is load-bearing for:

- profiling comparisons, which must hold the seed fixed
  (`AGENTS.md`, "Profiling hdEmbree");
- the project-wide image-validation convention (`doc/README.md:9`);
- `/home/anders/code/typhoon-tests`, which sweeps settings across fixed scenes;
- agents, which cannot edit a checked-in test stage to bisect a setting.

`--sessionLayer` technically covers all of this, but only by writing a temp
`.usda` file per variation. That is too much friction for a bisect loop and
cannot be expressed in a single shell command.

## Goals

- One repeatable option that sets any attribute on any **defined** prim in the
  stage — schema-defined attributes, already-authored attributes, and, with an
  explicit type, new custom attributes.
- Values use real usda syntax, parsed by the real parser, so every `SdfValueType`
  works without per-type command-line code — but parsed in isolation, so a value
  can only ever produce a value.
- A short, shell-safe token for "the render settings prim this invocation will
  actually use", so recipes are copy-pasteable across scenes.
- Fail loudly and early on a typo. A silently-ignored override is worse than no
  override at all, because the render still succeeds and produces a wrong-looking
  image.

## Non-goals

- No new hdEmbree code, tokens, schema attributes, or render settings.
- Not a general stage-editing language: no prim creation, `del`, metadata,
  relationships, variant selections, time samples, or references. Those keep
  using `--sessionLayer`. This boundary is **enforced**, not merely documented —
  see "Applying".
- Not added to `usdview` or `usdrecord` in this plan. `usdview` has the renderLab
  UI, and `usdrecord` is legacy (`AGENTS.md`, "Rendering And Output"). Both could
  reuse the module later; neither is in scope.
- No reinstatement of any `HDEMBREE_*` variable. This is the replacement, not a
  rollback.
- The hdEmbree code-size gate (`AGENTS.md:19`) does not apply — this is net-new
  capability in a different package, not an abstraction over existing code.

# Design

## Option name: `-s` / `--set`

Free short options today are `-o` and `-s`. Existing short options are `-c`,
`-f`, `-r`, `-w`, plus `-rp` and `-rs`, which are rewritten to their long forms
before parsing (`options.cpp:32`), so neither candidate collides.

**Take `-s`, reject `-o`.** `usdrender`'s primary job is writing image files and
it already has `--outputRoot`; `-o "int /X.y = 1"` reads as an output path, and a
user who types `-o /tmp/out` meaning `--outputRoot` gets a parse error from a
completely unrelated option. `-s`/`--set` describes the operation and collides
with nothing. `--sessionLayer` has no short form, so there is no confusion there
either.

## Target token: `{settings}`, not `$SETTINGS`

`$SETTINGS` is unusable as specified. The documented invocation quotes the
argument in double quotes, and both bash and zsh perform parameter expansion
inside double quotes. With `SETTINGS` unset — the normal case — the shell
substitutes the empty string and `usdrender` receives
`"int .ty:maxBounces = 12"`. That is a silent corruption of the argument the
program never sees, and it is unfixable inside `usdrender`.

Candidates considered:

| Form | Verdict |
| --- | --- |
| `$SETTINGS` | Expanded by bash/zsh inside the quotes actually used. Rejected. |
| `%SETTINGS%` | Shell-safe, but reads as a Windows idiom on a Linux-first tool. |
| `<settings>` | Safe inside quotes, but `<`/`>` are redirection operators, so any unquoted or partially-quoted use breaks. |
| `@settings` | Safe, short, but `@...@` already means "asset path" in usda; a second `@` meaning invites confusion. |
| `{settings}` | Safe in bash, zsh, fish, and PowerShell — brace expansion does not apply to a single-element brace and never applies inside quotes. Extends naturally to `{camera}`, `{pass}` later. **Recommended.** |

Match `{settings}` case-insensitively; agents will type `{SETTINGS}`.

The token is replaced with the resolved render settings prim path and is only
valid as a whole path prefix — it stands for a prim path, so `{settings}` and
`{settings}/Child` both work, and `x{settings}` is an error rather than a
substring substitution.

### No implicit bare-leading-`.` target

Do not add `.ty:maxBounces` as sugar for the render settings prim. An empty prim
path is too implicit in a command that also targets arbitrary prims, and it
creates a second spelling every parser diagnostic and test must explain.
`{settings}.ty:maxBounces` remains the one explicit, shell-safe form.

## Spec grammar

```
spec       := [ variability ] [ typeName ] target '=' valueText
variability:= 'uniform' | 'varying'
target     := primPath '.' attributeName
```

- `primPath` is an absolute `SdfPath` or `{settings}`-prefixed.
- `attributeName` is everything after the first `.`, namespaces included
  (`ty:maxBounces`, `primvars:displayColor`). Attribute names cannot contain
  `.`, so splitting on the first `.` is unambiguous.
- `valueText` is handed to the usda parser **in isolation** (see "Applying").
  **usda syntax, not C++ or Python syntax** — booleans are `true`/`false`, not
  `1`/`0`; strings are `"quoted"`; assets are `@path@`; arrays are `[1, 2, 3]`;
  tuples are `(1, 2, 3)`. For example,
  `bool ty:enableAdaptiveSampling = 1` must be written `= false` / `= true`.

### Relative asset paths follow standard resolution — document it, do not invent it

`@textures/wood.exr@` is a legal value, and its resolution is fully specified by
existing USD behavior. Nothing here needs designing; it needs writing down,
because the precedence is surprising.

The opinion lands in an anonymous layer, so there is no anchor:
`SdfComputeAssetPathRelativeToLayer()` takes the `anchor->IsAnonymous()` branch
and leaves the authored path unanchored (`sdf/layerUtils.cpp:189`). `Ar` then
resolves that identifier against the bound resolver context. With
`ArDefaultResolver` (`ar/defaultResolver.cpp:193-215`) the order is:

1. the **current working directory**;
2. search paths from the bound resolver context;
3. default search paths.

`usdrender` binds `CreateDefaultContextForAsset(o.usdFile)`
(`renderRequest.cpp:16`), so step 2 normally includes the root stage's directory.
Net effect for `--set`:

- `@textures/wood.exr@` — a *search* path. Tries `$PWD/textures/wood.exr` first,
  then the stage directory's `textures/wood.exr`. Working directory wins over the
  stage, which is the surprising part and the reason to document it.
- `@./textures/wood.exr@` — not a search path (`_IsSearchPath` excludes `./` and
  `../`), so it is checked against `$PWD` only. This is the form to use when the
  intent is "relative to where I am standing".
- `--resolverContext inherit` swaps in `CreateDefaultContext()`, which does not
  carry the stage directory, so the step-2 fallback disappears.
- A custom resolver may do something else entirely.

So the rule for `--help` is one line — relative asset values resolve like any
other unanchored asset path, working directory first — plus a recommendation to
use absolute paths when it matters. No special casing in `overrides.cpp`.

### `typeName` is optional, and that is the point

When the attribute is defined by the target prim's composed schema — which is
true of every `ty:` attribute, since `TyphoonRenderSettingsAPI` is an applied API
schema — resolve both the type name and the variability from
`prim.GetAttribute(name)` and let the user omit them:

```sh
-s "{settings}.ty:maxBounces = 12"
```

This is not just brevity. It removes the two mistakes that would otherwise
silently or confusingly fail:

- **Variability mismatch.** Every `ty:` attribute is `uniform`
  (`schema/schema.usda:48,52,84`). Authoring a `varying` spec over a `uniform`
  schema attribute is a composition error, and `int ty:maxBounces = 12` — the
  form in the original request — is exactly that spec.
- **Type mismatch.** `float ty:maxBounces = 12` authors a spec whose type
  disagrees with the schema.

### Three cases, and custom-attribute creation is one of them

Resolve `prim.GetAttribute(attributeName)` on the composed stage, before
authoring anything:

| Attribute state | Type source | Explicit type |
| --- | --- | --- |
| Schema-defined (every `ty:` attribute) | schema | optional; **error on mismatch** |
| Not schema-defined but already authored | composed opinion | optional; **error on mismatch** |
| Neither | the explicit type; **required** | authored `custom` |

The third row is load-bearing, not a generality reflex.
`domeLightCameraVisibility` — hdEmbree's dome-light camera visibility control —
is deliberately *not* a `ty:` schema attribute. It is a core
`HdRenderSettingsTokens` concept authored as a custom, unnamespaced attribute
(`testHdEmbreeRenderSettings.cpp:396` creates it with `CreateAttribute`), and
`renderRequest.cpp:44` harvests it by matching exactly
`IsCustom() && GetNamespace().IsEmpty()`. Refusing to create custom attributes
would make one of hdEmbree's documented render settings the single setting
`--set` cannot reach.

So the rule is: **the prim must exist and be defined; the attribute need not.**
Without an explicit type in the third case, error naming the attribute and asking
for one, since there is nothing to infer from.

Because rows one and two both supply a type, an explicit type is in practice an
*assertion* rather than a declaration everywhere except when creating a custom
attribute. That is the useful reading, and `--help` should present it that way.

Variability follows the same rule. For an existing attribute, omitted
variability comes from the composed attribute and explicit variability is an
assertion: reject `varying` against a composed `uniform` attribute and vice
versa. For a new custom attribute, use the explicit variability when present;
when omitted, use `SdfVariabilityVarying`, matching ordinary usda attribute
syntax.

## Where the overrides go

`LoadStage()` (`renderRequest.cpp:15-22`) currently builds the session layer as
either `SdfLayer::CreateAnonymous()` or `SdfLayer::FindOrOpen(o.sessionLayer)`.

Two constraints force the shape of the change:

1. **Never author into the user's `--sessionLayer`.** `FindOrOpen` returns a
   layer backed by a real file on disk. Authoring into it makes the file dirty
   and any subsequent `Save()` — by this process or another holding the same
   layer from the registry — writes the invocation's overrides into the user's
   asset.
2. **Type resolution needs the composed stage.** Reading
   `prim.GetAttribute(name).GetTypeName()` requires the stage to be open, and
   `{settings}` resolution requires stage metadata.

So: keep opening the stage first, then author into an anonymous layer.

- With no `-s`, behavior is byte-for-byte what it is today.
- With `-s` and no `--sessionLayer`, author into the existing anonymous session
  layer.
- With `-s` **and** `--sessionLayer`, the session becomes a fresh anonymous layer
  whose single `subLayerPaths` entry is the user's layer. Sublayers are weakest-
  last, so the anonymous root holds the overrides and wins over the user's
  session layer — which is the correct precedence for a command-line override —
  and the user's file is never touched.

  This only survives because overrides are authored through the `Sdf` API rather
  than by importing generated text. `SdfLayer::ImportFromString()` replaces a
  layer's *entire* content, `subLayerPaths` included, so importing into the
  wrapper would silently drop the user's `--sessionLayer` from composition. The
  "Applying" design below never imports into the destination layer, which removes
  the hazard rather than ordering around it.

## Resolution order for `{settings}`

`_Settings()` (`renderRequest.cpp:23-27`) resolves, in order:
`--renderSettingsPrimPath`, then `--renderPassPrimPath`'s render source, then the
stage's `renderSettingsPrimPath` metadata.

Extract it so both `LoadStage()`'s override step and `BuildRenderRequest()` call
the same function. Resolve **once, before applying overrides**, and state the
consequence in `--help` and the docs:

> `{settings}` is resolved against the stage as loaded, before any `--set`
> override is applied. Using `--set` to change the stage's
> `renderSettingsPrimPath` metadata and using `{settings}` in the same
> invocation is not supported.

If `{settings}` appears in any spec and resolution yields an empty path, fail
with `"--set used {settings} but no RenderSettings prim was specified or authored
in stage metadata"` rather than authoring an over at an empty path.

## Applying: parse each value in a scratch layer, then copy the value

Do **not** hand-parse values. There is no public API to parse a value string
against an `SdfValueTypeName`, and hand-rolling one would mean reimplementing
arrays, tuples, asset paths, quoting, and escapes — the exact set of things a
general override option exists to support. The real parser has to be used.

But it must not be pointed at a document the user shares with anyone else. The
obvious implementation — group all specs into one generated usda blob and
`ImportFromString()` it — splices `valueText` verbatim into a document whose
structure carries meaning. A value of

```
12
}
over "Other" { custom string injected = "yes" }
over "Ignored"
{
```

closes the attribute, closes the prim, authors a second prim, and reopens a block
so the generated tail still parses. That is prim creation, and prim creation is a
stated non-goal on line 3 of this plan's non-goals. The boundary has to be
enforced by construction, not by asking users not to do it.

**Two stages, and only a `VtValue` crosses between them.**

*Stage one — parse one value in isolation.* Per spec, build a minimal document in
a throwaway anonymous layer:

```
#usda 1.0
over "_"
{
    uniform int _value = <valueText>
}
```

`ImportFromString()` it, then assert the scratch layer is exactly what was asked
for and nothing else:

- one root prim spec, named `_`, with no children;
- exactly one property spec on it, named `_value`, and it is an attribute;
- no relationships, no attribute metadata, no time samples;
- no sublayers and no layer metadata.

Any deviation means the value text carried structure, and the spec is rejected
with "value contains additional layer content" plus the offending `-s` argument.
Then take `SdfAttributeSpec::GetDefaultValue()` — a `VtValue`. The scratch layer
is discarded.

*Stage two — author the value.* Nothing textual reaches the session layer:

- `SdfCreatePrimInLayer(session, primPath)` builds the ancestor spec chain, whose
  specs default to `SdfSpecifierOver` — exactly the `over` nesting the generated
  text was producing by hand;
- `SdfAttributeSpec::New(primSpec, attributeName, typeName, variability)` once
  per validated, de-duplicated target, with `SetCustom(true)` for the
  custom-attribute case;
- `SetDefaultValue(value)`.

What this buys beyond closing the boundary:

- **Per-argument diagnostics for free.** One `-s` argument in, one scratch layer
  parsed, one error attributable to it. The generated-blob design had to
  reverse-map parser line numbers in a document the user never wrote.
- **Duplicate `-s` for one attribute is last-value-wins after validation.**
  Validate and parse every argument against the unmodified composed stage,
  then group by normalized property path before authoring. Duplicate
  declarations must resolve to the same type and variability; conflicting
  declarations are an error. Retain only the last parsed `VtValue` and create
  one destination `SdfAttributeSpec`, avoiding a second
  `SdfAttributeSpec::New()` for an already-created property.
- **`--sessionLayer` survives**, because `subLayerPaths` on the destination is
  never touched.
- Type and variability come from the resolved attribute when it exists. A new
  custom attribute takes its required type and optional variability from the
  validated user declaration.

The one thing lost is that the scratch document's type must be known before the
value parses — which it is, since type resolution happens first.

The scratch document still interpolates two other fields, so validate both before
building it, not after: the type name against
`SdfSchema::GetInstance().FindType()`, and the attribute name against
`SdfPath::IsValidNamespacedIdentifier()`. Neither is a value, so neither gets the
benefit of the isolation argument above.

## Validate *before* authoring — and check `IsDefined()`, not `bool(prim)`

An `over` on a prim path that does not exist composes to a prim that is present
but undefined. Nothing errors, the render succeeds, and the setting is silently
ignored. For an agent bisecting a setting this is the worst possible failure
mode: it looks like the setting has no effect.

The check must run **before** authoring, and it must be the right check. Once
`over "Missing" { uniform int ty:x = 1 }` exists in the session layer,
`stage->GetPrimAtPath("/Missing")` returns a *valid* `UsdPrim` — USD instantiates
prims for over-only composition, which is precisely why `UsdPrim::IsDefined()`
exists and why the default traversal predicate filters on it. The attribute is
likewise valid, defined, and carries an authored opinion. So a post-apply pass
using ordinary existence or authorship queries reports success on exactly the
typos it was written to catch. Validating first sidesteps this, and the ordering
is forced anyway: type and variability resolution (above) must read the *unmodified*
composed stage.

Per spec, fail non-zero before anything is authored:

- **`!prim || !prim.IsDefined()`** → error naming the path. Both halves are
  needed: `!prim` catches a path composing to nothing at all, `!IsDefined()`
  catches a path that resolves only through some other over. This is the check
  for `/Render/Setings`, the single most likely typo.
- **no resolved attribute and no explicit type** → error naming the attribute.
  Enough to spot `ty:maxBounce`; a suggestion engine is scope creep.
- **explicit type disagrees with the resolved type** → error showing both.
- **explicit variability disagrees with the resolved variability** → error
  showing both.
- **duplicate declarations resolve to different types or variabilities** →
  error naming both offending `-s` arguments.
- **prim is an instance proxy** (`prim.IsInstanceProxy()`) → error. Cheap after
  all, given the prim handle is already in hand, and it converts the one
  known-silent case below into a loud one.

Because validation now precedes authoring, a failure leaves the session layer
untouched, so a partially-applied override set is not a reachable state.

The instance-proxy case was originally left as a documented non-check. Moving
validation ahead of authoring makes it a one-call test on a handle already
resolved, so there is no longer a reason to leave a second silently-ignored path
in place. Render settings prims are never instanced, so this matters only for the
arbitrary-prim uses.

## `--printOverrides`

A flag that, when at least one `--set` is present, dumps the generated session
layer via `ExportToString()` and continues. Roughly ten lines, and it turns "why
did my override not apply" from a guess into a read. Worth having given the
intended audience is largely agents.

# Implementation sequence

1. Add `overrides.h` / `overrides.cpp` to `usdrender`, and to `target_sources` in
   its `CMakeLists.txt`. Contents: the spec struct, spec parsing, `{settings}`
   expansion, stage-side prim/attribute validation and type resolution, isolated
   scratch-layer value parsing, and `Sdf`-API authoring.
2. `options.h` / `options.cpp`: add `std::vector<std::string> setSpecs` and
   `bool printOverrides`; register `app.add_option("--set,-s", ...)` and
   `app.add_flag("--printOverrides", ...)`. CLI11 accumulates repeated options
   into a vector automatically.
3. `renderRequest.cpp`: extract `_Settings()` into a shared
   `ResolveRenderSettingsPath()`, and **store its result in `StageData`** so it is
   computed exactly once. `LoadStage()` needs it for `{settings}`;
   `BuildRenderRequest()` then reads the stored path instead of resolving again.
   Two calls would not merely be wasteful — the second runs against a stage the
   overrides have already modified, which is the precise inconsistency the
   "resolved before overrides are applied" rule exists to prevent.
4. `renderRequest.cpp`: restructure `LoadStage()` for the three session-layer
   cases above and call into `overrides.cpp` after the stage opens. Validate,
   author, print if requested, and return false on any failure so `main()`
   (`usdrender.cpp:14`) exits 1 before rendering.
5. Tests (below).
6. Documentation, in the same change:
   - `AGENTS.md` "Profiling hdEmbree" — replace `HDEMBREE_RANDOM_NUMBER_SEED=1`
     in the `perf stat`, `perf record`, and trace recipes with
     `-s "{settings}.ty:randomNumberSeed = 1"`. Keep the note that
     scene-authored `ty:` settings take precedence over defaults, and add that
     `--set` takes precedence over both.
   - `doc/README.md:9` — the project-wide validation convention. Highest-leverage
     single line in the tree; every plan inherits it.
   - `README.md`, `OPTIMIZATION.md` — same substitution wherever an
     `HDEMBREE_*` variable is used as the mechanism.
   - `usdrender --help` text: grammar, usda value syntax, `{settings}`
     resolution timing, and the native-instance limitation.
7. **Launch an adversarial review agent over the new tests**, per the project
   testing rule, and address or discuss its findings before landing. The test set
   here is unusually easy to write vacuously — see below — so this step is not a
   formality.
8. Update plan 12: its step 12 currently says to replace fixed-seed recipes with
   "an authored `ty:randomNumberSeed` on the active `RenderSettings` prim". Once
   11 exists that becomes `--set`, and 12's risk bullet about breaking
   out-of-repo harnesses gains a stated migration path.

Note on style: `usdrender`'s existing sources are written as dense one-line
statements. A text parser with per-spec error reporting should **not** follow
that; write `overrides.cpp` conventionally. Flagging it because it makes the new
file visibly inconsistent with its neighbours — worth a decision either way
before implementing.

# Validation

### The existing fixture cannot carry these tests

`testenv/CpuFrames/test.usda` is one `Cube`, one `Camera`, and a
`RenderProduct` at 64×32, rendered with `--enableCameraLight`. Two consequences
kill the obvious test plan:

- **`ty:maxBounces` has nothing to act on.** A single convex object lit by a
  camera light has no interreflection, so `maxBounces = 0` and the default very
  likely produce the same pixels. A passing "override changes the image" test
  here would be measuring nothing, and a *failing* one would be indistinguishable
  from the feature being broken.
- **A fixed-seed reproducibility test is vacuous.** Plan 12 records that
  `randomNumberSeed = -1` derives the seed from the scene frame, so two runs at
  the same frame are already bit-identical with no `-s` at all. The test would
  pass with the entire feature stubbed out.

So: **add a `testenv/SetOverrides` fixture** — a diffuse box interior with a rect
light, at low sample count. That gives genuine interreflection (so bounce count
is observable) and genuine sample noise (so seed is observable). Reuse
`CpuFrames` only for the no-`-s` regression.

### Behavioral

- **No `-s` is byte-identical to today**, on `CpuFrames`. The regression that
  matters, since session-layer construction changes shape.
- **Structural override, renderer-independent.**
  `-s "/Render/Product.resolution = (32, 16)"` produces a 32×16 image. No image
  comparison, no fixture sensitivity, no renderer behavior in the loop — it is
  the cheapest unambiguous proof that a `--set` value reaches composition. Make
  this the first test written.
- **Bounce count is observable.** On `SetOverrides`,
  `-s "{settings}.ty:maxBounces = 0"` differs from `-s "{settings}.ty:maxBounces = 8"`.
  Assert the two *differ*; do not assert either against a baseline, which would
  re-encode current shading behavior.
- **Seeds differ, and a seed repeats.** `ty:randomNumberSeed = 1` vs `= 2` must
  produce different images — this is what proves the override applied. `= 1`
  twice must be bit-identical — this is the reproducibility guarantee replacing
  `HDEMBREE_RANDOM_NUMBER_SEED`. Both halves are required; only the first has
  any power to fail.
- **A non-settings prim works.** Override `/Camera.focalLength` on `CpuFrames`
  and confirm the framing changes. `{settings}` is convenience; arbitrary prims
  are the feature.
- **A custom attribute is created.** `-s "bool {settings}.domeLightCameraVisibility = false"`
  reaches `customSettings` (`renderRequest.cpp:44`), which requires the authored
  spec to be `custom` and unnamespaced. Directly covers the third type-resolution
  case.
- **`--sessionLayer` is not modified and not dropped.** Run with both
  `--sessionLayer` and `-s`. Assert the file's contents are unchanged on disk,
  that the override applied, **and that an unrelated opinion authored only in the
  user's session layer still composes** — the last clause is the one that catches
  a regression to `ImportFromString()` on the wrapper.
- **Sublayer precedence.** A `--sessionLayer` authoring `ty:maxBounces = 2` plus
  `-s "{settings}.ty:maxBounces = 8"` renders as 8.
- **A relative asset value resolves.** One case: a search-path `@...@` value
  naming a texture in the stage's directory, run from elsewhere, resolves via the
  resolver context. Enough to confirm `--set` authors a real `SdfAssetPath` and
  nothing swallows it; the resolution order itself is `Ar`'s to test, not this
  plan's.

Failure modes, each asserting exit code 1 and a message naming the offending
`-s` argument:

- unparseable spec (no `=`, no `.`, empty target);
- nonexistent prim path;
- a path that composes to an *undefined* prim — author `over "X" {}` in a
  `--sessionLayer` and target `/X`. This is the case a post-apply check would
  wave through, so it must be tested explicitly rather than assumed covered by
  the nonexistent-path case;
- attribute neither schema-defined nor authored, with no explicit type;
- explicit type disagreeing with the resolved type;
- explicit variability disagreeing with the resolved variability;
- duplicate declarations for one target with conflicting type or variability;
- unknown type token (`flt /X.y = 1`);
- malformed value text (`= [1, 2`);
- **value text carrying structure** — `-s '{settings}.ty:maxBounces = 12
  }
  over "Injected" { custom string x = "y" }
  over "Z" {'` must be rejected, and `/Injected` must not exist on the stage.
  The boundary is only real if something checks it;
- `{settings}` used with no resolvable RenderSettings prim;
- a target inside a native instance.

And: **a failed spec leaves the session layer empty.** Pass one valid and one
invalid `-s`; assert exit 1 and that the valid one was not authored.

Plus: duplicate compatible `-s` arguments for one attribute take the last
value and author exactly one destination attribute spec; `{SETTINGS}` and
`{settings}` behave identically.

# Risks and decisions

- **`-s` vs `-o`.** Recommending `-s`; the original request said `-o`. Flip by
  changing one string if you disagree, but `-o` next to `--outputRoot` in a tool
  that writes files is a real trap.
- **`{settings}` vs `$SETTINGS`.** Not a preference. `$SETTINGS` is silently
  corrupted by the shell in the documented invocation form.
- **usda value syntax surprises.** `= 1` for a bool and `= 0.5 0.5 0.5` for a
  color are the two most likely mistakes. Both produce a parse error rather than
  a wrong value, which is the acceptable outcome, but the `--help` text should
  show a bool and a tuple example explicitly.
- **Relative asset values resolve with the working directory ahead of the stage
  directory.** This is stock `ArDefaultResolver` behavior, not a choice this plan
  makes, so there is nothing to decide — but it is worth a `--help` line, since
  the same string authored inside the stage would find the stage's copy first.
- **Precedence is now three-deep**: schema fallback < authored stage opinion <
  `--set`. That is one more rule than plan 12 wanted to have. It is justified
  because it is *visible in the command line* — the specific property the
  environment path lacked — and it is per-invocation rather than ambient process
  state.
- Widening beyond attributes (prim creation, metadata, `del`) will be requested.
  Hold the line: `--sessionLayer` already does all of it, and the moment `--set`
  grows a second grammar it stops being a thing you can type from memory. The
  scratch-layer design makes the line structural — widening it would take a
  deliberate edit, not a clever value.
- **A new test fixture is required**, which is more than a doc-only plan usually
  costs. The alternative is tests that pass against a stubbed feature; that is
  not an alternative.

# Completion criteria

- `usdrender -s "<spec>"` authors attribute overrides for any prim in the stage,
  repeatable, with usda-syntax values.
- `{settings}` resolves to the render settings prim the invocation will use, and
  errors when there is none.
- Type and variability come from the composed stage when not written explicitly;
  an explicit type or variability that disagrees is an error; a typed custom
  attribute can be created, defaulting to `varying` when variability is omitted,
  so `domeLightCameraVisibility` is reachable.
- Compatible duplicate targets author one attribute spec and use the last
  value; conflicting duplicate type or variability declarations fail before
  authoring.
- A `--set` value can only ever produce a value. Value text carrying layer
  structure is rejected, and no `--set` invocation can create a prim.
- Every listed failure mode exits non-zero with the offending `-s` text in the
  message, is detected **before** anything is authored, and leaves the session
  layer untouched. No override is silently ignored — including undefined prims
  and instance proxies, both of which are checked rather than documented.
- `--sessionLayer` is never mutated, never dropped from composition, and
  overrides compose stronger than it.
- Invocations without `-s` produce bit-identical images to before the change.
- The render settings prim path is resolved exactly once, before overrides are
  applied, and stored in `StageData`.
- Tests run against a fixture in which the overridden settings are actually
  observable, and an adversarial review agent has passed over them.
- `AGENTS.md`, `README.md`, `OPTIMIZATION.md`, and `doc/README.md` state `--set`
  as the mechanism for fixed-seed validation and settings sweeps, so plan 12 can
  remove the environment path without removing the capability.

## Mandatory final suite gate

After every plan-specific validation above, run the complete Typhoon suite as
the final gate:

```sh
cd ~/code/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run the complete suite to completion; never interrupt it because of elapsed time.
All tests must pass. Report the total elapsed time. Runtime is variable: warn
when it exceeds 250 seconds, but timing alone does not fail the gate. Do not
commit the plan implementation until Anders has reviewed the completed changes
and explicitly approved committing them.
