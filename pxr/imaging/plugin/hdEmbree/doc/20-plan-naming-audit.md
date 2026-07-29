# Plan: Complete the repository-wide naming audit

Status: final rename-only readability sweep. Execute after
`19-plan-ty-namespace.md`, when behavior, source layout, and namespaces are
stable, and before `21-plan-api-contracts.md` documents the final names. This
plan closes the naming schema introduced by `05-plan-naming-core.md`; it must
not change rendered behavior.

## Scope

Generate an identifier inventory across all first-party `renderer/`,
`delegate/`, unconstrained MaterialXCpp C++, and `testenv/` C++ tests. Exclude
vendored BSDL, generated code, external struct fields, and authored schema/port
tokens.

Audit these families explicitly:

- every single-letter variable or parameter (`N`, `n`, `w`, `L`, `p`, `t`,
  `f`, `g`, `q`, `u`, `v`, `x`, `y`, `i`, `j`, `k`, and uppercase variants);
  `u1` / `u2` uniform random samples, `u` / `v` direct texture or
  surface-parametric coordinates, and `x` / `y` pixel column and row indices
  are explicit exceptions. `c` is also allowed for a pixel or RGB channel
  index when that role is clear from context. A
  single-letter local is also allowed in a short
  function when it is an obvious abbreviation of an already clearly named
  input or intermediate, has no competing meaning in scope, and is consumed
  immediately, such as `r` for clamped `frontReflectance` in
  `_ThinWalledWindowReflectance`, or `g1` through `g4` and `gOverPi` for the
  fitted intermediate terms in `_FonDirectionalAlbedoApprox`. This also
  permits `c` for the locally clamped `cosTheta` in
  `LookupGgxMissingEnergy`; expanding it to `valueC` adds no semantic
  information. Do not apply that exception when the value persists through a
  longer function: `_LookupBsdlDielectricBothMissingEnergy` uses
  `clampedCosTheta`, not the opaque `c` or `valueC`. `F0` is the standard
  literature notation for normal-incidence Fresnel reflectance.
  Within a microfacet BSDF, `D` is allowed for the normal-distribution term
  and `G` for the masking-shadowing term. Within conductor Fresnel
  calculations, `n` and `k` are allowed for the real and imaginary refractive
  indices. `fg` and `bg` are allowed for the foreground and background
  operands of a mix operation;
- every compound abbreviation derived from a single-letter root (`Ng`, `wi`,
  `wo`, `Li`, `Le`, `pdfW`, `invPdfA`, etc.). Use `pos` as the quantity root
  for position and `dir` as the quantity root for direction, with quantity
  first and semantic role/space suffixes such as `posHitWld`, `dirRayWld`,
  `posEntryWld`, and `dirExitWld`. Use `diffRay` for a `RayDifferential`
  value, keeping the ray family symmetric with `posRayOrg` and `dirRay`.
  Use `bary` as the prefix for barycentric-coordinate components, such as
  `baryU`, `baryV`, and `baryW`. Output pointer and reference parameters use
  the established `outFoo` convention; an `Out` suffix within `foo` retains
  its transport-side meaning;
- every spatial value missing its `Obj`, `Wld`, `Tangent`, `Texture`, or
  named-local-frame suffix at a shared-state or function boundary;
- every `In`/`Out` suffix, verifying that it means the incident/exitant
  transport side and not exterior orientation;
- every IOR/eta value, verifying that `iorIn` / `iorOut` are absolute indices
  and `eta` is their ratio;
- every normal, radiance, PDF, throughput, and medium-coefficient name against
  the naming table established by `05-plan-naming-core.md`.
- test fixture, helper, and assertion-local identifiers in `testenv/`, while
  preserving external API names and authored tokens used by the tests.

Known remaining rename families include `_SurfaceInteraction::p`, residual
role-first names such as `hitPos`, and ambiguous bare `dir` parameters. Use
the `pos` and `dir` quantity roots, then append the actual role and space from
their consumers; do not perform blind substitutions. For example, a
Russian-roulette `q` becomes `probabilitySurvival`. Ray parameter `t` is
explicitly allowed, as is `t` for a conventional local interpolation factor.
The `i`, `j`, `k` loop indices are allowed as long as they don't carry
additional semantic meaning other than the index in the array.
`idx` is allowed for an obvious local index when a more specific name adds no
useful information.
Bare `u` and `v` are allowed only when they directly denote texture or
surface-parametric coordinates.

Record each necessary exception beside its declaration with the external
constraint that requires it. Land one semantic rename family per commit.

## Validation

- Every rename-only commit is bit-identical under `oiiotool --diff` with the
  fixed authored `ty:randomNumberSeed`. Exercise front/back double-sided,
  normal-mapped, glass, SSS, lit, volume, and dielectric scenes.
- `pixi run ctest --test-dir build -R 'testHdEmbree|testMaterialXCpp' --output-on-failure`
  passes.
- Run an identifier-aware scan over first-party production and test code. It
  must find no
  unexplained single-letter identifiers, retired compound names, space-less
  shared spatial values, or snake_case SSS identifiers. Do not use a blanket
  textual grep that mistakes equations, comments, external fields, authored
  tokens, or vendored code for first-party identifiers.
- Verify normal role/space/orientation suffixes, omega direction/space
  suffixes, IOR side suffixes, and PDF measures at every declaration and call
  site.

## Documentation

Update the authoritative table and surrounding convention in
`ARCHITECTURE.md` with any audited cases or justified exceptions. Keep the
summary in `AGENTS.md` aligned. `21-plan-api-contracts.md` then documents the
final declarations; the documentation-role cleanup remains
`22-plan-documentation-roles.md`.

## Mandatory final suite gate

After every plan-specific validation above, run the complete Typhoon suite as
the final gate:

```sh
cd /path/to/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

Run the complete suite to completion; never interrupt it because of elapsed
time. All tests must pass. Report elapsed time. For a performance-sensitive
change, compare the same workload before and after on the same machine and
investigate regressions. Do not commit the plan implementation until your
human has reviewed the completed changes and explicitly approved
committing them.
