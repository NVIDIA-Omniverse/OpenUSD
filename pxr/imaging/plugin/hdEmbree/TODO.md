# hdEmbree TODO

## Material fidelity

- Replace the Schlick approximation used for dielectric reflection with exact
  dielectric Fresnel, or otherwise match MaterialX OSL/BSDL. Schlick can be
  too reflective at very grazing angles and is a likely contributor to the
  stronger specular highlights seen in the OpenPBR fuzz-weight sweep.

- Implement and select the Zeltner sheen model when `SheenMode::Zeltner` is
  requested. OpenPBR currently records the Zeltner mode, but sheen evaluation
  does not consult it and instead uses the existing Charlie/Conty-style lobe.
  MaterialX's OSL generator also drops the sheen `mode` argument, so OSL
  reference images may not expose this mismatch.

## Volume priorities and exact dielectric Fresnel

Implement exact dielectric Fresnel and priority-based nested-volume tracking
as one coordinated change. Nested media require the incident and transmitted
IORs at every real interface, so replacing Schlick independently with another
vacuum-relative calculation would leave the transport model incomplete.

The design follows the interior-list approach from Schmidt and Budge's
"Simple Nested Dielectrics in Ray Traced Images", also used by renderers such
as Arnold. Geometry may overlap, but only the highest-priority active logical
volume controls IOR and participating-medium transport. Intersections that do
not change the dominant logical volume are false interfaces: update the
interior state, continue the ray, and do not shade or consume a bounce.

### Authored data and semantics

Add two integer primvars:

```usda
int[] primvars:ty:volumeId = [1] (
    interpolation = "constant"
)
int[] primvars:ty:volumePriority = [10] (
    interpolation = "constant"
)
```

- `volumeId` identifies one logical volume. All closed geometry sharing an ID
  is treated as a CSG union.
- `volumePriority` chooses the dominant volume where different IDs overlap.
  Use the convention that higher numbers have higher priority.
- A volume ID has one canonical priority, IOR, and participating-medium
  definition. Diagnose conflicting definitions encountered for the same ID.
- Untagged geometry retains legacy behavior and always creates a real surface
  hit. This keeps opaque objects and existing scenes working without changes.
- Different IDs with equal priorities have a deterministic tie-break and emit
  a warning. Do not attempt to average unlike IORs or medium coefficients in
  the initial implementation.
- Initially support constant integer primvars. Add uniform/per-face values
  after the constant path works. Constant values must respect USD namespace
  inheritance and scene instancing.
- Explicit IDs are logical scene-wide IDs. Disjoint instances with the same ID
  are harmless; overlapping instances intentionally form one union. Geometry
  that must remain distinct needs distinct IDs.

### Interior state and boundary classification

Replace the single `HdEmbreeMediumState` owner pointer with a small interior
list. Use inline storage for the common case and permit overflow rather than
silently truncating valid nesting.

Each active entry records at least:

```cpp
struct HdEmbreeVolumeEntry {
    int volumeId;
    int priority;
    int windingCount;
    float ior;
    mxcpp::MediumProperties medium;
};
```

At every tagged boundary:

1. Resolve the ID, priority, absolute IOR, and interior medium.
2. Classify entry or exit using the unflipped geometric face normal, never the
   shading or normal-mapped normal.
3. Construct the candidate interior state after crossing. Entry increments
   the logical volume's winding count; exit decrements it.
4. Compare the dominant logical volume before and after the candidate update.
5. If the dominant ID is unchanged, treat the hit as a false interface:
   commit the candidate state, continue tracing, and perform no surface
   shading.
6. If the dominant ID changes, treat it as a real interface. Shade using the
   media on both sides, but commit the candidate state only if the sampled
   event crosses the boundary. Reflection and total internal reflection retain
   the original state.

Winding counts are required to make overlapping geometry with the same ID a
union. For example:

| Event | Count | Dominant medium | Interface |
| --- | ---: | --- | --- |
| Enter ball | 1 | Ball | Real |
| Enter an overlapping support | 2 | Ball | False |
| Exit the ball inside the support | 1 | Ball | False |
| Exit the support | 0 | Air | Real |

This is the intended fix for the hard internal boundaries currently visible
in shader-ball transmission scattering and random-walk SSS.

Add diagnostics and counters for negative winding counts, incompatible
properties sharing an ID, equal-priority overlaps, maximum interior-list depth,
and excessive consecutive false intersections.

### Geometry metadata plumbing

- Cache volume metadata in `HdEmbreePrototypeContext` beside the existing
  primvar samplers.
- Resolve constant metadata once during mesh sync. For later uniform support,
  resolve the coarse-face value at the Embree hit using `primitiveParams` so
  triangulation does not change authored IDs.
- Include instance context when resolving inherited/overridden values.
- Provide one hit helper that returns geometric identity, volume metadata,
  unflipped face orientation, material, and evaluated interior properties.
  Main paths, visibility rays, and SSS must use the same classification rules.

### Main path traversal and medium transport

- Add an inner intersection loop within each path bounce. It repeatedly traces
  past false interfaces until it reaches a real surface, a medium event, a
  light, or the environment.
- Evaluate absorption and free-flight scattering over each segment using the
  currently dominant volume.
- When a false interface leaves the dominant volume unchanged, continuing with
  a newly sampled exponential distance is valid by memorylessness, but use a
  distinct false-boundary sample-domain index to avoid repeating correlated
  random values.
- False interfaces do not consume a surface bounce, alter ray direction,
  perturb ray differentials, or introduce surface tint/roughness.
- Add a generous safety limit for consecutive false hits and report when it is
  reached rather than looping indefinitely on malformed or coincident geometry.
- Preserve existing behavior for thin-walled materials; they do not define an
  interior volume or alter the list.
- Clear dielectrics must still participate even when their
  `MediumProperties` are vacuum, because their IOR affects nested refraction.

### Exact Fresnel and nested IORs

Store absolute IOR on every logical volume; the default exterior is vacuum/air
with IOR 1.0. At a real interface, derive `etaIncident` and `etaTransmitted`
from the dominant entries before and after the crossing.

- Refactor dielectric BSDF evaluation and sampling to accept the IOR pair, or
  an explicitly derived relative eta, instead of assuming every boundary is
  material-to-vacuum.
- Replace Schlick dielectric reflection with exact unpolarized dielectric
  Fresnel using those two IORs.
- Use the same eta pair for Fresnel, Snell refraction, GGX transmission,
  dispersion, total internal reflection, delta transmission, and sampling
  probabilities.
- Ensure entering and exiting paths agree and that sampled throughput matches
  direct evaluation/PDF calculations.
- Keep conductor Fresnel and deliberate Schlick-based material models separate
  from this dielectric change.
- Revisit the BSDL rough-transmission energy LUT lookup once relative IOR is
  explicit. Its canonical table coordinate must use the correct ratio for the
  current pair of media on either side.

### Visibility and transparent shadows

Replace the shadow path's single copied medium with a copy of the full interior
tracker.

- Integrate transmittance piecewise as the dominant medium changes along the
  shadow segment.
- Update the list and skip false interfaces using the same classifier as main
  paths.
- Keep opaque and untagged surfaces as real blockers.
- Preserve the optional straight-through transparent-shadow approximation,
  but make its medium bookkeeping and surface-tint decisions priority-aware.
- Ensure a lower-priority proxy boundary never adds a tint or visibility loss
  while hidden inside a higher-priority volume.

### Random-walk subsurface scattering

The SSS walker currently treats any hit on the same Embree geometry as an exit.
Pass the logical volume ID and initial winding state into the walk instead.

- Continue tracing through same-ID internal boundaries while the winding count
  remains positive.
- Return an SSS exit only when the logical union's count reaches zero.
- Preserve the exact final primitive and barycentrics for the synthetic
  Lambertian exit hit.
- Apply priority resolution if the walk enters a distinct tagged volume.
- Use unflipped face normals for all winding updates.
- Keep the existing random-walk coefficient and phase-function logic unchanged
  until boundary tracking is independently validated.

### Initial state for cameras inside volumes

Do not assume primary rays begin in air. Seed a reusable tracker for the camera
position by casting a deterministic probe from outside the scene bounds to the
camera and replaying tagged boundary crossings. Cache it per camera and time
sample. Secondary, SSS, and visibility rays inherit their parent state.

Document closed, consistently oriented geometry as the supported authoring
model. Emit useful diagnostics for open or inconsistently wound tagged meshes,
since no priority algorithm can infer a reliable interior from arbitrary
non-manifold boundaries.

### Tests

First unit-test the renderer-independent interior-list state machine:

- A single closed volume.
- Overlapping shells with one ID behaving as a union.
- Distinct IDs with different priorities.
- Entering and exiting a low-priority volume while a higher-priority volume is
  active.
- Reflection and total internal reflection leaving state unchanged.
- Successful transmission committing the candidate state.
- False intersections updating state without consuming bounces.
- A cavity reducing the winding count to zero at its real boundary.
- Equal-priority and malformed-winding diagnostics.
- Interior-list inline-capacity overflow without loss of correctness.

Then add renderer integration scenes for:

- Two overlapping same-ID spheres with no internal interface.
- Glass, liquid, ice, and air with distinct priorities and absolute IORs.
- Nested absorption and anisotropic scattering.
- Nested-media visibility and transparent shadows.
- Instanced and inherited volume IDs.
- Camera origins inside tagged volumes.
- Uniform IDs surviving triangulation and subdivision/refinement mapping.
- The material-fidelity shader ball, verifying that the transmission-scatter
  and SSS hard lines disappear when its overlapping pieces share one ID.
- Exact Fresnel at normal incidence, grazing incidence, both travel
  directions, equal IORs, and the total-internal-reflection threshold.
- Sample/eval/PDF consistency for nested rough and delta dielectrics.

After the tests are implemented, launch the required adversarial test review to
check that they validate intended transport behavior rather than encoding the
new implementation's current output.

### Rollout, debugging, and performance

- Add a temporary render-setting or environment switch so the priority path
  can be compared against legacy single-medium tracking during development.
- Add debug AOVs or logging for dominant volume ID, priority, winding depth,
  and false-interface count.
- Profile interior-list operations, false-hit recasts, material evaluation at
  hidden proxy boundaries, and visibility traversal.
- Avoid evaluating full surface closures for known exits where cached volume
  metadata is sufficient. False entries still need their interior material
  properties, which may be spatially varying.
- Retain the switch for one release cycle if existing scenes show unexpected
  changes, then make priority tracking the default for authored volume IDs.

### Suggested commit sequence

1. Add volume primvar ingestion and the tested interior-list state machine.
2. Integrate false-interface traversal and dominant-medium selection into main
   paths.
3. Implement exact dielectric Fresnel and explicit incident/transmitted IORs.
4. Make visibility and transparent shadows priority-aware.
5. Make SSS exit logical volume unions instead of raw Embree geometry.
6. Add camera-inside initialization, integration fixtures, diagnostics,
   documentation, and profiling results.

### References

- Schmidt and Budge, "Simple Nested Dielectrics in Ray Traced Images":
  https://escholarship.org/content/qt9h83s3m0/qt9h83s3m0_noSplash_693fb7df0433335e068723e435d6a371.pdf
- Arnold nested-dielectric documentation:
  https://help.autodesk.com/cloudhelp/ENU/AR-Core/files/ac-shading/ac-surface-shaders/ac-standard-surface/ac-transmission/arnold_user_guide_ac_transmission_ac_nested_dielectrics_html.html
- Takua's implementation notes:
  https://blog.yiningkarlli.com/2019/05/nested-dielectrics.html
- USD primvar interpolation and inheritance:
  https://openusd.org/release/user_guides/primvars.html
