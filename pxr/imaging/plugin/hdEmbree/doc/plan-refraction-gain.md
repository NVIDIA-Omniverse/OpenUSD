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

## Option 2: revert to stock BSDL and move our lobe onto it

Keep `use_bvn_refraction = true` and rebake nothing. Drop the local additions to
the vendored tree, sample and weight the coupled dielectric the way
`DielectricBSDF` does, and apply the stock `Both` tables the way `DielectricLobe`
applies them. The end state is that no file under `renderer/materials/BSDL/`
differs from upstream, and every table we read describes the lobe we evaluate.

This is option 3's transport change taken to its conclusion: option 3 stops at
sampling and leaves the compensation form open; this option also reverts the
local bake types and commits to BSDL's multiplicative compensation. Option 3's
objections apply here too and are restated under Cost and risk.

### What reverting removes

The local patch is additive. Since before `9b9641f55`, `renderer/materials/BSDL/`
is +36/-0 in `MTX/bsdf_dielectric_decl.h`, +134/-0 in
`MTX/bsdf_dielectric_impl.h`, +92/-2 in `src/genluts.cpp` and +3/-1 in
`bsdl.cmake`; the two `genluts.cpp` deletions are the templating of
`bake_emiss_tables` on `store_energy`. It adds `DielectricTransFront/Back` and a
second bake mode and changes nothing BSDL evaluates or samples, so reverting it
is not itself a fix for the gain — it is what makes the tree verbatim.

It does have one consequence to plan for. Reverting deletes
`MTX/bsdf_dielectric_trans{front,back}_luts.h`, and with them the source of
`dielectricTransmissionLut.h` and
`LookupBsdlDielectricTransmissionSingleScatterAlbedo`. That lookup feeds
`Bsdf::CoupledRoughDielectricDirectionalTransmissionAlbedo`, the approximate
transparent-shadow albedo in `bsdf.cpp`. Stock BSDL tabulates no equivalent: it
splits nothing out of the `Both` tables, and `DielectricReflFront` is the
reflection-only dielectric with no back-side companion. The bake moves to the
MaterialXCpp side, driving stock BSDL through its public API; step 5 has the
detail.

### Steps

1. **Revert the vendored tree.** `git revert` the BSDL hunks of `9b9641f55` and
   `da2bf2d3d`: the two bake types in `MTX/bsdf_dielectric_{decl,impl}.h`,
   `compute_transmission_E` and the `store_energy` template parameter in
   `genluts.cpp`, and the two `_bsdl_lut_headers` entries in `bsdl.cmake`.
   Confirm with `git diff` against upstream that nothing under
   `renderer/materials/BSDL/` remains local.
2. **`microfacet.h`** — port `GGXDist::sample_for_refl` and `GGXDist::D_refl_D`
   from `renderer/materials/BSDL/include/BSDL/microfacet_tools_impl.h`, listings
   1 and 2 of Eto and Tokuyoshi. Both are anisotropic in `ax`/`ay` already, so
   they drop into the anisotropic path without a second isotropic variant.
3. **`dielectric.cpp` sampling and pdf.** `SampleCoupledRoughDielectric` draws
   its microfacet normal from `sample_for_refl` instead of
   `SampleGGX_VNDF_Anisotropic`; `PdfCoupledRoughDielectric` replaces
   `PdfGGX_VNDF_Anisotropic` with `D_refl_D * D`, on both the reflection and the
   transmission branch, matching `DielectricBSDF::eval`'s
   `D_refl / (4 cosNO) * F.max()` and `D_refl * J * Ft.max()`. The two must move
   together: the returned pdf is what the integrator's MIS weights use, so a
   sampler on one density and a pdf on another is worse than either.
4. **The eval functions do not change.** Under `use_bvn_refraction`, BSDL's
   `out * pdf` still equals `f * |cos|`; the bounded VNDF moves the proposal
   density only, not the BSDF value. Assert that identity in
   `testMaterialXCppBsdf.cpp` rather than assuming it, for both branches.
5. **Bake the transparent-shadow albedo MaterialXCpp-side, through BSDL's public
   API.** A small generator beside the MaterialXCpp tests, linking `BSDL::BSDL`
   and including `BSDL/microfacet_tools_{decl,impl}.h` and
   `BSDL/MTX/bsdf_dielectric_{decl,impl}.h` in that order, constructs
   `bsdl::mtx::DielectricBSDF<DielectricFresnel>` from `GGXDist(roughness,
   aniso, flip)`, `DielectricFresnel(eta, backfacing)` and `cosNO` with
   `dorefr = true`, and estimates

   ```
   E_trans = mean over samples of (s.wi.z < 0 ? s.weight.max() : 0),
             s = bsdf.sample(wo, u1, u2, u3)
   ```

   That is the transmission half of exactly what the runtime sampler delivers,
   bounded VNDF included, so the table describes the transported lobe rather
   than an idealised one. `testMaterialXCppBsdf.cpp` already instantiates
   `DielectricBSDF` this way, so the include order and the link are proven; no
   new BSDL type, no `genluts.cpp` change, nothing vendored is touched.

   Consume it as `E_trans / max(0.01f, 1 - E_ms)`, the same scale transport
   applies. Because `E_refl + E_trans == 1 - E_ms` by construction — all three
   are means over the same sampler — the shadow albedo is at most one without a
   clamp, and it agrees with transport at every grid point instead of to within
   a percent or two.

   Bake it on the `Both` tables' axes: `DielectricBSDF::get_cosine(c)`, linear
   perceptual roughness, `DielectricFresnel::table_index()`. The old table used
   a `sqrt` roughness axis only because `bake_emiss_tables` squares the index
   under `store_energy`; owning the bake means one axis convention, so the new
   lookup reuses `_LookupBsdlDielectricBothMissingEnergy`'s coordinate code
   instead of a second variant of it.

   Follow the existing convention for the output: a generator target that is not
   part of the default build, writing a checked-in
   `renderer/materials/MaterialXCpp/materials/bsdf/dielectricTransmissionLut.h`
   with a header comment naming the generator and the sampler it measured.
6. **`energyCompensation.{h,cpp}`** — `BsdlCoupledDielectricCompensation`
   returns the missing energy alone. Delete `reflectionRatio`,
   `_AverageFresnelDielectric` and `_AverageBsdlDielectricBothMissingEnergy`: the
   four-point Gauss-Legendre average and the average-Fresnel split exist solely
   to aim the added cosine lobe, and a multiplicative compensation never needs to
   know which side the missing energy belongs to. `bsdf.cpp`'s
   `CoupledRoughDielectricDirectionalTransmissionAlbedo` follows: the
   `albedo += missingEnergy * (1 - reflectionRatio)` line becomes the step 5
   scale. Its exact-Fresnel branch below `kTransmissionExactFresnelMaxAlpha` and
   the smooth blend above it are unaffected.
7. **`closureTraversal.cpp:461-478`** — delete the added-lobe block. Scale the
   coupled reflection and transmission contributions by
   `1 / max(0.01f, 1 - missingEnergy)`, which is `DielectricLobe::eval_impl`
   verbatim. Keep passing `!compensateCoupledDielectric` to
   `EvalMicrofacetReflection*`: the `Both` table covers the reflection branch, so
   the generic GGX compensation stays off there.
8. **`dielectric.cpp` compensation.** `SampleCoupledRoughDielectric` loses its
   cosine-lobe branch and its `u1` remap; `PdfCoupledRoughDielectric` loses
   `specularProbability` and `compensationPdf`. Compensation stops perturbing the
   pdf entirely, matching `DielectricLobe::sample_impl`, which scales the weight
   and leaves the pdf alone.
9. **Close the gap that hid this.** Add a byte-identity test for the `Both`
   tables mirroring the transmission one that step 1 deletes, in
   `testMaterialXCppBsdf.cpp`. There is currently no such test for `Both`, which
   is why the table and the lobe were never compared.

### Validation

Same as option 1, plus:

- `weight * pdf == f * |cos|` for both branches under bounded VNDF, as step 4;
- our sampled transmission albedo must now land on BSDL's sampler column of the
  measurement-3 table, not on the unbiased column, at every grid point. That is
  the check that says the two sides finally agree;
- `E_refl + E_trans == 1 - E_ms` from step 5's bake against the stock `Both`
  tables, to Monte-Carlo tolerance, at every grid point. It is the budget
  assertion for the shadow table and it is exact by construction, so a failure
  means the bake and the table disagree about the sampler;
- the four `openpbr_dielectic_r*` fixtures under a dome light, not only the
  budget quadrature. The compensation form and the pdf both moved, so a
  quadrature that only integrates `f * cos` cannot see a broken MIS weight.

### Cost and risk

- No table is rebaked and no vendored file stays modified, which is the point of
  the option. The cost lands entirely in transport, which is the largest blast
  radius of any option here: sampling, pdf, MIS weights and the compensation form
  all move at once.
- It imports the bias BSDL flags in its own comment. Directional transmission
  drops by up to 42% at roughness 1.0 and returns as a broader scaled lobe.
  Rough glass loses refraction and gains haze. **This is an accepted trade**:
  the policy is to run BSDL's model everywhere, in transport and in the shadow
  bake alike, rather than to keep a locally corrected lobe that no table
  describes.
- **Furnace closure under NEE still has to be measured.** `D_refl_D` is finite
  for every microfacet normal, so the bounded-VNDF pdf is the analytic
  continuation of a truncated density: it returns a positive pdf for normals the
  sampler can never produce. Under BSDF sampling alone the compensation closes,
  because `E_ms` measured exactly what the sampler misses. Under NEE plus MIS,
  eval still delivers those refracted directions weighted by `p_L / (p_L + p_B)`
  with a `p_B` no strategy realises, so the weights across strategies do not sum
  to one there. Measure the dome-light furnace; do not infer closure from the
  budget table. If it does not close, the residual is in the MIS weight, not the
  compensation, and the consistent fix is to truncate `eval` to the same cap the
  sampler uses: invert `sample_for_refl`'s stretch, `m_std ∝ (m.x / ax, m.y / ay,
  m.z)` and `o_std = 2 (i_std . m_hat) m_hat - i_std`, and return zero when
  `o_std.z < -k * i_std.z`. The lobe is then genuinely truncated on both sides,
  its albedo is exactly `1 - E_ms`, and closure holds under any estimator, at the
  cost of a hard edge at grazing.
- Scaling instead of adding removes the non-reciprocity listed as out of scope,
  and takes a branch out of a hot sampling path.

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

## Decision

**Option 2.** The policy is to run BSDL's model everywhere — bounded-VNDF
sampling in transport, BSDL's multiplicative compensation, and a shadow table
baked through BSDL's public API against the same sampler — rather than to keep a
locally corrected lobe that no upstream table describes. The refraction bias
BSDL documents is accepted, in exchange for one lobe with one owner and a
vendored tree that is verbatim upstream.

Options 1 and 3 are kept above as the alternatives that were rejected, and as
the record of why. Option 1 is correct and is the fallback if option 2's
dome-light furnace cannot be made to close, but it grows the local patch and
leaves our lobe unlike anything upstream bakes. Option 3 is option 2's transport
half without the revert or the compensation change, so it is subsumed rather
than rejected; its first step is still worth doing alone as the diagnostic
cross-check if the attribution ever needs nailing down harder than the four
measurements above already do.

Two things land regardless of which option is taken:

- the byte-identity test for the `Both` tables, which is missing and which is
  the reason a table baked against a different lobe went unnoticed;
- a budget assertion comparing the lobe against the table rather than either
  against itself, which is the check that catches this whole class of defect in
  one run. Note that its statement is option-dependent, and that
  `TestCoupledRoughDielectricCompensationBudget` currently quadratures the
  production integrand, so it asserts the option 1 form. Under option 1 it is
  `A_single + missingEnergy == 1` to about 1% on that quadrature. Under option 2
  the quadrature exceeds `1 - E_ms` by design, because eval is unbounded while
  the sampler is capped; the equivalent assertion is
  `E_refl + E_trans == 1 - E_ms` over the bounded-VNDF sampler, plus the
  dome-light furnace to cover what the sampler and the evaluator disagree about.

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
