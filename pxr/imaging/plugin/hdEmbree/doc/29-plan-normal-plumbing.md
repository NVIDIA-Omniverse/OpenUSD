# Plan: Simplify normal plumbing in closure traversal

Status: proposed simplification follow-up to
[`28-plan-bump-map-correction.md`](28-plan-bump-map-correction.md). Plan 28 is
correctness work and is still landing; this plan is behavior-preserving cleanup
of the plumbing that plan 28 grew. Do not start it until plan 28's image
acceptance and adversarial test review are complete, or the two will fight over
the same signatures.

Line numbers below are a snapshot of the plan 28 working tree and will drift.

## Problem

Plan 28 added a smooth normal, a geometric normal, and a bump-shadowing context
to a traversal that already carried a graph normal, an outgoing direction, a
hero wavelength, and a geometric side. Nothing is wrong with the values; the
problem is that they are all threaded by hand through a mutually recursive walk.

Current counts in non-test renderer code:

| Identifier | Occurrences |
| --- | --- |
| `normalShdWldOut` | 433 |
| `normalShdLobeWldOut` | 274 |
| `normalSrfWldOut` | 126 |
| `normalGeomWldOut` | 76 |
| `normalGeomWldExt` | 72 |
| `normalSrfWldExt` | 48 |
| `normalShdWldExt` | 32 |

Within `bsdf/closureTraversal.cpp` alone: 134 `normalShdWldOut`, 51
`normalSrfWldOut`, 135 `frontFacing`, 100 `heroWavelengthNm`, 14 `bumpContext`,
and 38 `ResolveTreeShadingNormal` call sites.

The traversal entry points now carry these parameter counts:

| Function | Parameters | Per-hit invariants among them |
| --- | --- | --- |
| `EvalNode` | 9 | 6 |
| `EvalNodeCosine` | 9 | 6 |
| `SampleNode` | 11 | 6 |
| `PdfNode` | 7 | 4 |
| `_ApproxWeight` | 6 | 4 |
| `_EvalThroughput` | 6 | 4 |

**Every parameter except `nodeId`, `omegaInWld`, and the three random numbers is
constant for the whole hit.** `normalShdWldOut`, `normalSrfWldOut`,
`normalGeomWldOut`, `omegaOutWld`, `heroWavelengthNm`, `frontFacing`, and
`bumpContext` are computed once at the hit and then copied down every recursive
edge. That is the whole complexity: seven invariants, hand-threaded.

Two consequences beyond verbosity:

- Positional aliasing is representable. Three of the invariants share the `Vec3f`
  type, so passing the wrong one compiles silently. `TryEvalPdfAdobeOpenPbrSurface`
  (`adobeOpenPbr.cpp:822`) still passes `normalShdWldOut` into the
  `normalGeomWldOut` slot and hard-codes `frontFacing = true`.
- Adding one invariant is an N-file edit. `bumpContext` was the most recent; the
  next one will cost the same.

## What is irreducible

Do not try to reduce the number of distinct normals. Four are genuinely distinct
physical quantities with different owners, recorded in `ARCHITECTURE.md`:

| Normal | Owns |
| --- | --- |
| `normalGeomWld*` | topology, medium transitions, ray offsets, sample-side validity |
| `normalSrfWld*` | shadow terminator, bump agreement and diffuse softening |
| `normalShdWld*` | the mapped graph normal for the whole material |
| `normalShdLobeWldOut` | the interface one lobe describes |

`frontFacing` must also stay an independent input; plan 28's invariant forbids
reconstructing it from any shading-normal dot product.

## Already landed

Recorded so this plan does not re-propose them:

- The bump-shadowing agreement test and diffuse softening are now one shared
  pair, `BumpHemisphereAgreement()` and `BumpShadowingTerm()`, in
  `bsdf/microfacet.h:163-215`, with an explicit `BumpShadowingContext`
  enum. The three hand-rolled copies are gone.
- The compatibility overloads that defaulted the smooth and geometric normals to
  the shading normal have been removed from `bsdf.h`, `closureTraversal.h`, and
  `legacySurface.h`.
- `EvalNodeCosine()` is a thin wrapper over a shared `_EvalNode()` rather than a
  second traversal.

## Decision

Four changes, in dependency order. The first is the largest deletion and makes
the second smaller.

1. Resolve every surface leaf's normal once during preparation, and stop
   threading the graph normal through traversal at all.
2. Bundle the remaining per-hit invariants into one struct passed by const
   reference.
3. Consolidate the per-family sampled-direction validity test, which is still
   written three times.
4. Remove the subsurface normal side channel and the unused smooth normal on
   subsurface entry.

A fifth, keeping the exterior frame out of mxcpp entirely, is described but not
recommended for this pass.

## Implementation inventory

### 1. One prepared normal per leaf; drop the graph normal from traversal

`PrepareShadingNormals()` (`bsdf/closureTraversal.cpp:1949`) walks only the
authored-normal linked chain and writes `data.normal`. Leaves *without* an
authored normal instead resolve `defaultDiffuseNormal` or
`defaultSpecularNormal` at each of the 38 `ResolveTreeShadingNormal` call sites,
on every evaluation, PDF, approximate weight, throughput estimate, and sample —
many times per hit.

Change preparation to walk all nodes and write `data.normal` with
`hasShadingNormal = true` for every surface leaf, applying the same validation
order and the same corrected/uncorrected leaf-type split it already implements.
`ResolveTreeShadingNormal` then collapses to a `data.normal` read.

This deletes:

- `defaultDiffuseNormal`, `defaultSpecularNormal`, and
  `hasDefaultSpecularNormalNodes` (`closureTree.h:279-281`), their resets
  (`:291-294`), and the reflective bookkeeping in `_RecordNormalState`
  (`:341-371`);
- `firstAuthoredNormalNodeId` (`closureTree.h:277`) and the per-node
  `nextAuthoredNormalNodeId` (`:269`), with the `Add()` plumbing at `:310-330`;
- `ResolveTreeShadingNormal`'s diffuse/specular `if constexpr` classification and
  its `shadingNormalsPrepared` branch on the hot path
  (`closureTraversal.cpp:62-83`);
- `CausticClassPruner`'s default copying (`closureTraversal.cpp:110`ff);
- `defaultSpecularNormal` reads in `adobeOpenPbr.cpp`;
- **`normalShdWldOut` as a parameter of `EvalNode`, `EvalNodeCosine`, `PdfNode`,
  `SampleNode`, `_ApproxWeight`, `_EvalThroughput`, `_EvalLayerBaseThroughput`,
  and `_FinalizeSubtreeSample`.** It survives only as an input to
  `PrepareShadingNormals()` and to the deliberately unprepared standalone leaf
  helpers.

It also collapses a maintenance trap. Four type lists must currently agree:
`_HasSurfaceLobeNormal()` (`closureTraversal.cpp:86`),
`_IsDiffuseClosureFamily()` (`:102`), `ResolveTreeShadingNormal`'s specular list
(`:62`), and `PrepareShadingNormals`' corrected list (`:1949`). After this change
the corrected/uncorrected decision exists only in preparation, and the diffuse
family list only in the shadowing and validity helpers.

Safety: all three production entry points prepare before evaluating or sampling
— `pathIntegrator.cpp:381`, `surfaceShading.cpp:921`, `unlitIntegrator.cpp:135`
— so no production path reaches an unprepared tree. Keep
`ResolveShadingNormal(data, fallback)` in `bsdf/shadingFrame.h` for the
standalone leaf helpers that tests use, and keep `shadingNormalsPrepared` as the
assertion that distinguishes the two.

Performance: this trades one `Vec3f` store per leaf per hit against removing a
branch and a tree lookup at 38 sites, each reached once per evaluation, PDF,
approximate weight, and twice more inside `_FinalizeSubtreeSample`. The linked
chain was optimizing preparation, which runs once, at the expense of traversal,
which runs many times. Expect neutral-to-faster, but measure — see
[Required local validation](#required-local-validation).

### 2. One per-hit interaction struct

After section 1 the remaining invariants are `normalSrfWldOut`,
`normalGeomWldOut`, `omegaOutWld`, `heroWavelengthNm`, `frontFacing`, and
`bumpContext`. Introduce one struct in `bsdf/shadingFrame.h`, beside the
existing frame helpers:

```cpp
/// Per-hit state that is constant for one closure traversal. All normals and
/// `omegaOutWld` are finite unit vectors on the incident transport side.
/// `frontFacing` is immutable geometric state and must not be inferred from a
/// shading normal.
struct SurfaceInteraction
{
    Vec3f normalSrfWldOut;
    Vec3f normalGeomWldOut;
    Vec3f omegaOutWld;
    float heroWavelengthNm = 0.0f;
    bool  frontFacing = true;
};
```

Signatures become:

```cpp
Vec3f EvalNode(const ClosureTree&, NodeId, const SurfaceInteraction&,
               const Vec3f& omegaInWld, BumpShadowingContext);
Vec3f EvalNodeCosine(const ClosureTree&, NodeId, const SurfaceInteraction&,
                     const Vec3f& omegaInWld, BumpShadowingContext);
float PdfNode(const ClosureTree&, NodeId, const SurfaceInteraction&,
              const Vec3f& omegaInWld);
BsdfSample SampleNode(const ClosureTree&, NodeId, const SurfaceInteraction&,
                      float u1, float u2, float uChoice);
```

Keep `bumpContext` a separate argument rather than a struct field: it is the one
value that legitimately differs between two calls at the same hit, and burying it
in the interaction invites the evaluation/sampling mix-up that plan 28 had to fix.
`PdfNode` takes the struct but reads only `omegaOutWld`, `heroWavelengthNm`, and
`frontFacing`; that is the point — PDF stays independent of both the smooth and
geometric normals, and the contract says so rather than the signature implying it
by omission.

Named fields make positional aliasing unrepresentable, which is what actually
fixes `TryEvalPdfAdobeOpenPbrSurface` (`adobeOpenPbr.cpp:822`): it must construct
an interaction, and the caller at `lighting.cpp` has the real geometric normal and
`frontFacing` in scope.

Mirror the same struct through the public `Bsdf::EvalSurface`,
`Bsdf::EvalSurfaceCosine`, `Bsdf::PdfSurface`, `Bsdf::SampleSurface`, the legacy
entry points, and `AdobeOpenPbrPreparedSurfaceState`, which currently stores the
same values as five separate fields.

Do not add a constructor that defaults any normal from another. The absence of
such a shortcut is the property being bought.

### 3. One sampled-direction validity predicate

The per-family geometric rejection is written three times: the `if constexpr`
ladder in `SampleNode`'s post-visit (`closureTraversal.cpp`), the ladder in
`SamplePreparedAdobeOpenPbrSurface` (`adobeOpenPbr.cpp`), and
`finalizeReflection` in `SampleLegacySurface` (`legacySurface.cpp:197`). All three
now share `BumpHemisphereAgreement()` for the bump half but each re-derives the
geometric half.

Add one free function beside the bump helpers:

```cpp
enum class LobeFamily { Diffuse, Translucent, Microfacet };

/// Cycles-aligned sampled-direction validity. Diffuse and translucent test only
/// the geometric normal, matching bsdf_diffuse_sample and
/// bsdf_translucent_sample; microfacet tests both normals against the event
/// label, matching bsdf_microfacet_sample, and counts a tangent direction as
/// reflection.
bool SampledDirectionIsValid(
    LobeFamily family, bool isTransmission,
    const Vec3f& normalShdLobeWldOut, const Vec3f& normalGeomWldOut,
    const Vec3f& omegaOutWld, const Vec3f& omegaInWld);
```

Taking `isTransmission` as an explicit input also closes a defect plan 28 left
open: the traversal currently re-derives the label from
`!IsSameSide(normalShdLobeWldOut, omegaInWld, omegaOutWld)` after sampling, which
makes the lobe-normal half of the microfacet test a tautology. The leaves already
set the flag correctly at the source — `SampleGGXTransmission` in `bsdf.cpp`, the
delta helpers in `dielectric.cpp` — and the post-visit overwrites it. Deleting the
re-derivation restores Cycles' `cos_NO` check and removes code.

### 4. Remove the subsurface side channel

`BsdfSample::normalShdLobeWldOut` exists so a subsurface marker can hand its lobe
normal to `sss.cpp`. It is live on one sample kind, meaningless on every other,
and its header contract has to say so. After section 1 the prepared normal is in
the tree, so `_SubsurfaceInput` (`renderer.h:700-702`) can carry the closure and
the selected node id instead, or read the subsurface leaf directly.

Weigh this one: the side channel is small and the replacement adds a node id to
`_SubsurfaceInput`. Adopt it only if section 1 makes the tree read clean;
otherwise leave the field and keep its contract.

Independently, drop the `normalSrfWldOut` parameter of
`Bsdf::SampleSubsurfaceEntry` (`bsdf.cpp:592`). It is validated on entry and then
never used; Cycles' `subsurface_entry_bounce` has no smooth-normal term.

### 5. Exterior frame at the mxcpp boundary — described, not recommended

`FaceNormalShdWldOut(normalShdWldExt, frontFacing)` is applied in
`pathIntegrator.cpp`, `surfaceShading.cpp`, and `unlitIntegrator.cpp`, and
`PrepareShadingNormals()` separately takes `normalShdWldExt` plus `frontFacing`
and re-derives the same flip internally. Two sources of truth for one orientation
decision, and the reason both orientations cross into the material layer (32
`normalShdWldExt` uses).

If authored lobe-normal validation moved next to the graph-normal resolution
step, where the tangent-space map is already evaluated in the exterior frame,
mxcpp would only ever see incident-side normals. That is a genuine simplification
but it touches the exact code path run 0239 got wrong, for a smaller payoff than
sections 1 to 3. Leave it unless a later change forces the area open again.

## Dead code removed

- `ClosureTree::defaultDiffuseNormal`, `defaultSpecularNormal`,
  `hasDefaultSpecularNormalNodes`, `firstAuthoredNormalNodeId`, and
  `Node::nextAuthoredNormalNodeId`.
- The reflective-type bookkeeping in `ClosureTree::_RecordNormalState`.
- `ResolveTreeShadingNormal`'s diffuse/specular classification.
- The `normalShdWldOut` parameter on eight traversal functions.
- Two of the three per-family sampled-direction validity ladders.
- The post-sample `isTransmission` re-derivation.
- `Bsdf::SampleSubsurfaceEntry`'s unused `normalSrfWldOut` parameter.

## Tests

This plan is behavior-preserving, so the primary evidence is that plan 28's
suite passes unchanged. Beyond that:

- keep every plan 28 assertion; none of its test bodies should need new values,
  only reconstruction of the interaction struct at the call site;
- add a preparation test that every surface leaf in a mixed tree — authored and
  unauthored, corrected and uncorrected families — carries a resolved
  `data.normal` after `PrepareShadingNormals()`, and that an unprepared tree
  still resolves through the standalone fallback;
- add a test that a closure pruned by `CausticClassPruner` retains its per-leaf
  prepared normals now that there are no tree-level defaults to copy;
- keep the existing subsurface entry-direction test working through whichever
  form section 4 chooses;
- assert that `PdfSurface` results are unchanged by the refactor for a fixed
  closure and direction set, since PDF must stay independent of the smooth and
  geometric normals.

Per `AGENTS.md`, launch an adversarial review agent once the tests are updated.
Its specific charge here is to confirm the tests would fail if a leaf normal were
left unresolved by preparation, and that no test constructs a `SurfaceInteraction`
whose three normals are all equal, which would silently restore the degenerate
configuration the deleted compatibility overloads used to allow.

## Implementation sequence

1. Land section 1 alone. It is the largest deletion and the only one with a
   plausible performance effect; keeping it a separate commit makes a regression
   bisectable.
2. Land section 2. Mechanical once section 1 has removed one of the invariants.
3. Land section 3. Pure deduplication plus the `isTransmission` fix, which is the
   one behavior change in this plan and needs its own note in the commit message.
4. Land section 4 if section 1 made the tree read clean; otherwise drop it and
   keep only the `SampleSubsurfaceEntry` parameter removal.
5. Update `ARCHITECTURE.md` where it describes closure normal ownership and the
   tree-level defaults. No `README.md` or `overview.dox` change: nothing
   user-visible or externally contracted changes.
6. Run the complete external gate and report elapsed time:

   ```sh
   cd /home/anders/code/typhoon-test-suite
   powerprofilesctl launch --profile performance -- \
       pixi run pytest --renderer typhoon-local
   ```

## Required local validation

From the OpenUSD checkout:

```sh
pixi run build
pixi run build/pxr/imaging/plugin/hdEmbree/testMaterialXCpp
pixi run ctest --test-dir build -R testHdEmbree --output-on-failure
```

Use the bare `testHdEmbree` filter rather than plan 28's narrower regex: that
regex matches only three of the six registered hdEmbree tests and silently skips
`testHdEmbreeWireframe`, `testHdEmbreeSubdivision`, and
`testHdEmbreeLightLinking`.

This plan is behavior-preserving, so every rendered difference must be explained,
not tolerated. Sections 1 and 2 both alter per-hit hot paths and follow the
investigation in [`27-plan-perf-regression.md`](27-plan-perf-regression.md).
Take the "before" measurement on a named fixed-seed, material-heavy workload
before section 1 lands; it is not recoverable afterwards. Section 1 is expected
to be neutral or slightly faster and section 2 neutral. Record durable measured
results in `OPTIMIZATION.md` if either moves materially in either direction.
