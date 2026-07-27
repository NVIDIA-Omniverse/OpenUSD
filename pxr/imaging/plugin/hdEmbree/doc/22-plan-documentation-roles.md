# Plan: Give README, ARCHITECTURE, AGENTS, and overview distinct roles

Status: documentation-maintenance cleanup.

## Issue

`AGENTS.md`, `ARCHITECTURE.md`, `README.md`, and `overview.dox` repeat directory
maps, render flows, subdivision/displacement behavior, settings behavior, and
extension guidance. `AGENTS.md` and `ARCHITECTURE.md` alone exceed 1,100 lines.
The maintenance rule currently requires all affected documents to stay current,
so duplicated detail increases every change's burden and creates drift risk.

The documents serve different readers, but their boundaries are not explicit.
Critical invariants can be lost inside long feature descriptions, while agents
must read a large amount of architecture duplicated elsewhere.

## Goals

- Assign one authoritative home to each kind of information.
- Keep cross-links instead of copied multi-paragraph explanations.
- Preserve all durable invariants and workflow knowledge.
- Make the maintenance rule precise about which document changes when.

## Proposed roles

### `README.md`

User-facing capabilities, supported workflows, authored settings/AOVs, visible
limitations, and short examples. No internal function/file maps.

### `ARCHITECTURE.md`

Authoritative developer design: dependency boundary, ownership, frame/path
flows, invariants, extension points, and responsibility map. Detailed renderer
behavior belongs here once.

### `AGENTS.md`

Project goals, mandatory coding rules, build/test/profile commands, review and
editing pitfalls, documentation-maintenance policy, and concise links to the
relevant architecture sections. Retain only architectural facts needed to avoid
unsafe edits before following a link.

### `overview.dox`

Short generated plugin overview covering Hydra integration and the runtime
contract — renderer and schema identities, `ty:` settings, AOVs. **Not an API
index:** plan `01` records that hdEmbree exposes no supported hand-written C++
API and removes every installed header from doxygen input, so `overview.dox` is
the plugin's only doxygen source. Do not mirror the complete internal
architecture, and distinguish real extension mechanisms (`plugInfo.json`
registration, the USD schema) from ordinary internal module boundaries.

### `OPTIMIZATION.md` and `TODO.md`

Measured performance history and actionable future work respectively; avoid
repeating current architecture except enough context to make an entry durable.

## Implementation sequence

1. Build a section-level duplication table across the four primary documents.
2. Choose the authoritative home for each duplicated topic.
3. Move/merge the best version before deleting duplicates so no invariant is
   lost.
4. Replace removed text with descriptive links, not “see elsewhere” without a
   target section.
5. Shorten the maintenance rule to require updating every *affected authority*,
   then list the roles above.
6. Check all paths, commands, setting names, and documented source owners
   against current code.
7. Add a short documentation map near the top of `AGENTS.md` and
   `ARCHITECTURE.md`.

## Validation

- Search distinctive invariant phrases and confirm each has one authoritative
  definition.
- Check every internal Markdown link and referenced file path.
- Have a reviewer answer: how to build/test, where a new setting belongs, how a
  pixel becomes a path, and what is user-visible—each should have one obvious
  document.
- Compare line counts only as a secondary signal; completeness matters more
  than arbitrary reduction.

## Risks and decisions

- `AGENTS.md` must remain self-sufficient enough to prevent unsafe edits when a
  reader does not immediately open architecture links.
- User-visible limitations belong in README even if architecture also explains
  their cause; link rather than duplicate mechanics.
- Avoid anchors that are likely to change frequently; use stable section names.

## Completion criteria

- Each document states its audience and authority.
- Long behavioral descriptions are not duplicated across AGENTS and
  ARCHITECTURE.
- The maintenance rule maps changes to documents unambiguously.
- All current behavior, invariants, workflows, and extension points remain
  documented.

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
