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
  `u1` / `u2` uniform random samples are the explicit exception;
- every compound abbreviation derived from a single-letter root (`Ng`, `wi`,
  `wo`, `Li`, `Le`, `hitPos`, `rayDir`, `entryPos`, `exitDir`, `pdfW`,
  `invPdfA`, etc.);
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

Known rename families include `_SurfaceInteraction::p`, all `hitPos` /
`entryPos` / `exitPos`, `_PathState::rayDir`, SSS `entryDir` / `exitDir`,
`LightSample::dist`, `_Visibility::dist`, and bare helper parameters such as
`dir`. Choose semantic replacements from their actual consumers; do not
perform blind substitutions. For example, ray `t` becomes
`distanceAlongRayWld`, a Russian-roulette `q` becomes
`probabilitySurvival`, and a channel loop `i` becomes `indexChannel`.

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
cd ~/code/typhoon-test-suite
powerprofilesctl launch --profile performance -- pixi run pytest --renderer typhoon-local
```

All tests must pass. The expected baseline is approximately 235 seconds. If any
test fails or runtime is 250 seconds or above, stop: do not continue or land
the plan. Check with Anders before proceeding.
