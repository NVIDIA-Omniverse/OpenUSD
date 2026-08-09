# Plan: the coupled rough dielectric gains energy

Status: **diagnosed, not fixed.** The cause is located exactly and three fixes
are laid out below. Nothing in the renderer has been changed.

The OpenPBR dielectric white furnace gains energy, monotonically in roughness.
Measured on the disk against a background of exactly 0.5, in
`test-suite/furnace/openpbr_dielectic_r*.usda` of the typhoon test suite:

| roughness | disk mean | vs background |
|---|---|---|
| 0.0 | 0.49997 | -0.01% |
| 0.2 | 0.50637 | **+1.27%** |
| 0.5 | 0.57544 | **+15.09%** |
| 1.0 | 0.84051 | **+68.10%** |

At roughness 1.0 the gain is largest at normal incidence, +87.7% at
`|N.V| = 0.99`, falling to +50.1% at `|N.V| = 0.46`.

---

## The defect

`BsdlCoupledDielectricCompensation` returns a `missingEnergy` that is not the
complement of the lobe we evaluate. We add a cosine lobe carrying that energy on
top of a lobe that is already full, so the interface's directional energy albedo
exceeds one at every crossing, and a solid dielectric compounds that over its
internal bounces.

The compensation contract is that

```
A_reflection + A_transmission + missingEnergy == 1
```

for the same directional albedos the evaluator actually produces. Measured on
the production integrand from `closureTraversal.cpp`, ior 1.5:

| side | roughness | cos | A_refl | A_trans | A_single | missingE | sum |
|---|---|---|---|---|---|---|---|
| front | 0.20 | 0.60 | 0.06490 | 0.93442 | 0.99931 | 0.00153 | 1.00084 |
| front | 0.60 | 0.85 | 0.03577 | 0.93761 | 0.97338 | 0.09430 | 1.06768 |
| front | 0.80 | 0.85 | 0.02481 | 0.89966 | 0.92447 | 0.26032 | 1.18479 |
| front | 1.00 | 0.85 | 0.01469 | 0.82913 | 0.84383 | 0.50259 | **1.34641** |
| back | 1.00 | 0.60 | 0.14408 | 0.36636 | 0.51044 | 0.59589 | 1.10633 |

Zero at low roughness, +35% per crossing at roughness 1. That is the furnace's
roughness ramp and its centre-bright radial profile.

### Where the mismatch comes from

`mtx::DielectricBothFront` and `mtx::DielectricBothBack` do not define their own
`sample()`. They inherit `DielectricBSDF<Fresnel>::sample`, which draws
microfacet normals with `d.sample_for_refl` — bounded VNDF, from Eto and
Tokuyoshi, optimised for **reflection** — then refracts through those normals
and discards any refraction landing back on the incident side.
`BSDL_CONFIG::use_bvn_refraction` is `true`
(`renderer/materials/BSDL/include/BSDL/config.h:100`). BSDL says what this costs,
in `MTX/bsdf_dielectric_impl.h`:

> This skips micro normals not valid for reflection, but they could be valid for
> refraction. Energy is ok because we renormalize this lobe, but **refraction
> will be biased for high roughness**. We trade that for reduced noise.

`genluts.cpp` bakes the `Both` tables with
`bake_emiss_tables<BSDF, store_energy = false>`, whose estimator `compute_E` is
purely sampler-driven — `E = mean(bsdf.sample(...).weight.max())` — and stores
`1 - E`. So `kBsdlDielectricBothFront/BackMissingEnergy` is

```
1 - (energy of a deliberately reflection-biased transmission lobe)
```

BSDL's own use is self-consistent. `DielectricLobe::eval_impl` does

```cpp
Sample s = spec.eval(wo, wi);
if (dorefr)
    s.weight *= 1 / std::max(0.01f, 1 - E_ms);
```

It scales its own short lobe back to unity. We took one half of that pair: an
unbiased lobe plus BSDL's compensation for a biased one.

### The evidence, in order

1. **The table is exactly the complement of BSDL's lobe.** Instantiating
   `bsdl::mtx::DielectricBSDF<DielectricFresnel>` and Monte-Carlo sampling it
   gives `bsdlLobe / (1 - missingEnergy) = 1.000` at all fifty grid points
   (0.997 to 1.002). The LUT is right and our lookup is right.
2. **Our reflection lobe is BSDL's**, to four decimals at every grid point, for
   example 0.01469 against 0.01470 at front roughness 1.0 `cos` 0.85, and
   0.38121 against 0.38098 at back roughness 1.0 `cos` 0.15.
3. **Only transmission diverges**, and it tracks roughness:

| side | roughness | cos | our A_trans | BSDL's sampler | ratio |
|---|---|---|---|---|---|
| front | 0.20 | 0.60 | 0.93442 | 0.93358 | 1.001 |
| front | 0.60 | 0.85 | 0.93761 | 0.87017 | 1.078 |
| front | 0.80 | 0.85 | 0.89966 | 0.71516 | 1.258 |
| front | 1.00 | 0.85 | 0.82913 | 0.48260 | **1.718** |

4. **BSDL's other table settles which is right.** `DielectricTransFront/Back`
   bake the same `eval` under an unbiased proposal. Against
   `LookupBsdlDielectricTransmissionSingleScatterAlbedo`, our transmission reads
   `ourT / unbiasedT = 1.000`, within 1 to 2 percent everywhere. Our lobe is
   BSDL's own unbiased answer; it is BSDL's runtime sampler that is short.

That last point also shows this class of defect was already found once and
fixed for the other table. `DielectricTransFront::sample` carries the comment

> Dividing by the uniform-hemisphere PDF integrates directional transmission
> energy **without inheriting the runtime sampler's reflection-optimized bias**.

The `Both` tables never got the same treatment.

### Ruled out — do not re-run

- **The table bytes.** `kBsdlDielectricBothFront/BackMissingEnergy` are
  bit-identical to BSDL's generated `MTX/bsdf_dielectric_both{front,back}_luts.h`.
  Max absolute difference 0 over all 8192 entries of each.
- **The lookup coordinates.** `DielectricBSDF::get_cosine(i)` is linear, unlike
  `MicrofacetMS`, which is quadratic; `TabulatedEnergyCurve::interpolate_emiss`
  is linear in perceptual roughness; `DielectricFresnel::table_index()` is
  `sqrt((eta - 1.001) / (5 - 1.001))`. `_LookupBsdlDielectricBothMissingEnergy`
  does exactly that. `GGXDist(rough)` sets `ax = SQR(rough)`, so the axis is
  perceptual roughness, and `BsdlLayerRoughnessFromAlpha` reduces algebraically
  to BSDL's `sqrt(ax / (1 + aniso))`.
- **The two tables' differing roughness axes are both correct.** In
  `bake_emiss_tables`, `bsdfRoughness = store_energy ? SQR(roughness_index)
  : roughness_index`. That is why `dielectricBothLut.h` is linear in perceptual
  roughness while `dielectricTransmissionLut.h` is `sqrt`, and why the two
  lookups in `energyCompensation.cpp` already differ. Neither is a bug.
- **The shared GGX compensation.** The conductor furnace is clean at the same
  four roughnesses: +0.00%, +0.00%, +0.10%, -0.26%. Only
  `transmission_weight = 1` gains, which is the only path through
  `GetCoupledDielectricCompensation`.
  `test-suite/rough_dielectric_white_furnace_sphere.usda` reads -0.11% but is
  **not** a control: despite the name it authors no transmission, so it
  exercises the same reflection-only path the conductor does.
- **Reciprocity.** A white furnace only requires directional albedo one; a
  non-reciprocal lobe with unit albedo is invisible in it. The added cosine lobe
  is non-reciprocal, and that is not what this defect is.
- **Add versus scale.** Adding `E` and dividing by `1 - E` are both correct when
  `A_single = 1 - E`. Ours is not, and dividing would be worse:
  `0.844 / 0.497 = 1.70` at front roughness 1.0, `cos` 0.85.

---

## Option 1: rebake the `Both` tables against an unbiased proposal

Fix it where it is wrong. Give `DielectricBothFront/Back` their own unbiased
bake, exactly as `DielectricTransFront/Back` already have, and regenerate.

### Why it is correct

The identity that makes the reweight trivial is that `eval()` returns a weight
and a pdf whose product is the BSDF value:

```
eval().weight * eval().pdf == f * |cos(theta_i)|
```

independently of which proposal `eval` assumed. So any proposal can be used and
converted, which is precisely what `DielectricTransFront::sample_importance`
does with `s.weight *= s.pdf / pdf`. After the rebake, `missingEnergy` becomes
the complement of the unbiased single-scattering albedo, which measurement 4
above shows is our lobe.

### Steps

1. **`renderer/materials/BSDL/include/BSDL/MTX/bsdf_dielectric_decl.h`** —
   declare `sample` and `sample_importance` on `DielectricBothFront` and
   `DielectricBothBack`. Note that in the vendored tree these two structs
   currently declare **no** sampling entry point at all — only the constructor,
   `lut_header`, `struct_name` and `get_energy` — which is exactly why
   `compute_E` reaches `DielectricBSDF::sample` and picks up the bounded-VNDF
   bias. Copy the two declarations verbatim from `DielectricTransFront`, which
   sits a few lines below and already has them. (The BSDL headers installed by
   the `openusd-typhoon` conda package differ here; the vendored tree under
   `renderer/materials/BSDL/` is what compiles, and it is the one to edit.)
2. **`renderer/materials/BSDL/include/BSDL/MTX/bsdf_dielectric_impl.h`** —
   define both, mirroring `DielectricTransFront`:
   - `sample`: a **full-sphere** uniform proposal, since `Both` covers
     reflection and transmission. Draw `z` in `[-1, 1]`, return
     `s.weight *= s.pdf * 4 * PI`, and set `s.pdf` to the matching
     visible-normal proposal density for MIS. The transmission variants use a
     hemisphere and `2 * PI`; do not copy that constant.
   - `sample_importance`: plain `d.sample(wo, randu, randv)`, both branches
     selected by Fresnel as `DielectricBSDF::sample` does but **without**
     `sample_for_refl`, then `s.weight *= s.pdf / pdf` against the plain
     visible-normal density.
   - Note that `DielectricBSDF::eval` branches on `use_bvn_refraction` for its
     own weight and pdf. That is fine and must not be touched: the reweight
     above cancels whichever convention it used.
3. **`renderer/materials/BSDL/src/genluts.cpp`** — add a `compute_both_E`, or
   generalise `compute_transmission_E` to the full sphere with the uniform pdf
   as a parameter. Keep the balance-heuristic MIS between the uniform and
   visible-normal proposals; it is what makes the near-smooth rows converge.
   Keep storing `1 - E` for the `Both` tables, so the table stays a
   missing-energy table and `_LookupBsdlDielectricBothMissingEnergy` is
   unchanged. Handle `roughness_index == 0` analytically, as
   `compute_transmission_E` already does: single scattering is exact there, so
   the stored value is 0.
4. **Regenerate.** No build plumbing is needed: `MTX/bsdf_dielectric_bothfront_luts.h`
   and `..._bothback_luts.h` are already in `_bsdl_lut_headers` in
   `renderer/materials/BSDL/bsdl.cmake`, and the `${NAME}_genluts` target
   rewrites them into `${_bsdl_generated_bsdl_dir}` on every build. Copy the
   values into
   `renderer/materials/MaterialXCpp/materials/bsdf/dielectricBothLut.h`,
   preserving its header comment and updating the axis note to say the bake is
   now unbiased.
5. **Close the gap that hid this.** Add a byte-identity test for the `Both`
   tables mirroring the existing transmission one at
   `testMaterialXCppBsdf.cpp` (search `kBsdlDielectricTransmissionFrontSingleScatterAlbedo`).
   There is currently **no** such test for `Both`, which is why a table baked
   against a different lobe was never noticed.

### Validation

- `TestCoupledRoughDielectricCompensationBudget` must read
  `A_single + missingEnergy` within 1% of 1 at every grid point. Convert it from
  a printing diagnostic into an assertion at that tolerance. The tolerance has
  to be tight because a solid dielectric compounds the per-crossing error over
  its internal bounces: 1% per crossing is several percent on a sphere.
- The four `openpbr_dielectic_r*` furnace fixtures within tolerance, and the
  four `openpbr_conductor_r*` unchanged.
- The full typhoon-test-suite gate.

### Cost and risk

- Bake cost is `128 * 128` samples over `16 * 16 * 32` entries for two tables,
  parallelised; minutes.
- Changing `DielectricBothFront::sample` also changes what BSDL's own
  `DielectricLobe` compensation would do. Nothing in hdEmbree instantiates
  `bsdl::mtx::DielectricLobe` — verified by grep — so the blast radius is the
  generated tables only.
- `AGENTS.md` forbids style churn in vendored BSDL. This is a functional patch,
  and `DielectricTransFront/Back` are already local additions, so there is
  precedent. Keep the patch minimal and comment why it exists.

---

## Option 2: derive the missing energy from tables we already have

Compute `missingEnergy = 1 - A_reflection - A_transmission` from tabulated
single-scatter albedos of the two lobes we actually evaluate, instead of reading
a missing-energy table baked against someone else's lobe.

### Why it is correct

It states the contract directly. It also has the pleasant property that half the
work is already done and already validated: `A_transmission` is
`LookupBsdlDielectricTransmissionSingleScatterAlbedo`, which measurement 4 shows
matches our transmission lobe to 1 to 2 percent, front and back.

### What is missing

`A_reflection` for the coupled interface, that is `E_vndf[F(cosMO) * G2/G1]`,
front and back. `DielectricReflFront` cannot supply it: it bakes the
**reflection-only** dielectric, whose `sample()` is
`sample_turquin_microms_reflection` and is therefore already compensated, and
there is no back-side companion.

### Steps

1. **`renderer/materials/BSDL/include/BSDL/MTX/bsdf_dielectric_{decl,impl}.h`** —
   add `DielectricReflOnlyFront` and `DielectricReflOnlyBack` bake types with
   `lut_header()` names of their own. Their `sample`/`sample_importance` follow
   the `DielectricTrans*` pattern but over the **upper** hemisphere and taking
   the reflection branch only, weighted by `F(cosMO)`.
2. **`renderer/materials/BSDL/src/genluts.cpp`** — add them to
   `BAKE_TRANSMISSION_ALBEDO_LIST`, which uses `store_energy = true` and stores
   `E` rather than `1 - E`. Note this list also squares the roughness index, so
   the new table's roughness axis is `sqrt(perceptual roughness)`, matching the
   transmission table's; the lookup must use `std::sqrt(roughness)` like
   `LookupBsdlDielectricTransmissionSingleScatterAlbedo` and not like
   `_LookupBsdlDielectricBothMissingEnergy`.
3. **New `renderer/materials/MaterialXCpp/materials/bsdf/dielectricReflectionAlbedoLut.h`**
   plus `LookupBsdlDielectricReflectionSingleScatterAlbedo` in
   `energyCompensation.{h,cpp}`, alongside the transmission lookup it mirrors.
4. **`energyCompensation.cpp`** — in `BsdlCoupledDielectricCompensation`, replace
   the `_LookupBsdlDielectricBothMissingEnergy` call with
   `Clamp01(1 - A_refl - A_trans)`. Two independently interpolated tables can sum
   to slightly over one near smooth, so the clamp is load-bearing, not
   decorative.
5. **Leave `reflectionRatio` alone.** It splits the *missing* energy between the
   two sides and is derived from average Fresnel; the split of present energy is
   a different quantity and substituting one for the other would be a new bug.
   `_AverageBsdlDielectricBothMissingEnergy` still reads the old `Both` table for
   that ratio, so the table stays in the build even though its direct use goes
   away. Decide explicitly whether to keep it or rebake it too.

### Validation

Same as option 1, plus: assert that `A_refl` from the new table matches the
budget diagnostic's measured `A_refl` column, which is already known to equal
BSDL's reflection to four decimals.

### Cost and risk

- Two table lookups per shade where there was one, on a path that already does
  several.
- Two independent interpolations mean the sum can drift from the true albedo in
  a way a single table cannot; the residual will not be identically zero at the
  grid points the way option 1's is.
- Still requires patching vendored BSDL and regenerating, so it is not cheaper
  than option 1 in the place that matters. Its advantage is that it reuses a
  table already proven against our lobe, and that the resulting quantity is by
  construction the complement of what we evaluate.

---

## Option 3: adopt bounded-VNDF refraction so our lobe becomes BSDL's

Change the renderer instead of the table: sample and evaluate the coupled
dielectric exactly as BSDL does, so the existing `Both` table applies unchanged.

### Steps

1. **`renderer/materials/MaterialXCpp/materials/bsdf/microfacet.h`** — port
   `GGXDist::sample_for_refl` and `GGXDist::D_refl_D` from
   `renderer/materials/BSDL/include/BSDL/microfacet_tools_impl.h`, which are
   listings 1 and 2 of Eto and Tokuyoshi's bounded VNDF paper.
2. **`dielectric.cpp`** — `SampleCoupledRoughDielectric` draws from
   `sample_for_refl`; `PdfCoupledRoughDielectric` uses the matching
   `D_refl_D * D` density instead of `PdfGGX_VNDF_Anisotropic`;
   `EvalCoupledRoughDielectricTransmission` picks up the `G1 / D_refl_D`
   reweight that BSDL's eval applies under `use_bvn_refraction`. All three must
   change together or `value / pdf` breaks.
3. Either keep adding `missingEnergy`, or switch to BSDL's
   `1 / max(0.01f, 1 - E_ms)` scaling. Both are then correct; scaling keeps the
   lobe shape, adding does not.

### Why this is the weakest option

- It knowingly imports a bias BSDL itself flags. Our transmission would drop by
  up to 42% at roughness 1.0, and the energy would come back as a cosine lobe
  rather than as refracted directions. Rough glass would lose directional
  transmission and gain diffuse haze. The furnace would pass while the image got
  worse.
- It touches sampling, evaluation and pdf in the production transport path,
  which is the largest blast radius of the three, and every MIS weight moves.
- The one thing it is good for is a **cross-check**: implementing
  `sample_for_refl` in the diagnostic only and confirming that our transmission
  albedo then drops onto BSDL's would close the last inferential step in the
  diagnosis above. That is cheap and does not require touching transport.

---

## Recommendation

**Option 1**, with option 2 as the fallback if patching the vendored bake is
judged unacceptable. Reject option 3 as a fix; consider its first step as a
diagnostic cross-check if the attribution needs to be nailed down harder than
the four measurements above already do.

Whichever is chosen, two things should land regardless:

- the byte-identity test for the `Both` tables, which is missing and which is
  the reason a table baked against a different lobe went unnoticed;
- the budget assertion, `A_single + missingEnergy == 1` to about 1%, which is
  the check that catches this whole class of defect in one run. Every existing
  test compared the lobe against itself or the table against itself, and none
  compared the two against each other.

## Reproducing

`TestCoupledRoughDielectricCompensationBudget` in
`renderer/materials/MaterialXCpp/tests/testMaterialXCppBsdf.cpp` prints the
whole budget: our quadrature of the production integrand, the analytic
visible-normal albedo, BSDL's own lobe split into reflection and transmission,
the `Both` table's `missingEnergy`, and the unbiased transmission table. It
prints and always passes.

```sh
cd /path/to/openusd-omniverse
pixi run build
./build/pxr/imaging/plugin/hdEmbree/testMaterialXCpp
```

It needs `BSDL/MTX/bsdf_dielectric_impl.h` and `BSDL/microfacet_tools_impl.h`,
included after the decl headers; including the impl first fails because
`Sample` is not yet declared.

For the furnace numbers, render
`test-suite/furnace/openpbr_dielectic_r{00,02,05,10}.usda` from the typhoon test
suite and compare the sphere disk against the background. The silhouette radius
is `50 * tan(asin(1 / 4.5)) / 18 * 128`, about 81 pixels at 256 by 256.

## Out of scope

- The non-reciprocity of the cosine compensation lobe. Real, and not what this
  plan is about; a furnace cannot see it.
- `reflectionRatio`, the split of missing energy between the two sides. It is
  derived from an average-Fresnel heuristic and has never been validated
  directly. Once the total is right, that split becomes the next thing worth
  measuring.
- Thin-walled and separate transmission, which do not take this path.
