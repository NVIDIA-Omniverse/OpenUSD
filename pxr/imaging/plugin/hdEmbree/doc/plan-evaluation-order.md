# Plan: Dependency-only MaterialX evaluation order

## Goal

Fix `mxcpp::EvalGraph::_EvaluateNodeOutput()` so nested input reevaluation executes only the requested node and its transitive upstream dependencies, in topological order. Preserve shifted-`ShadingContext` semantics, nested scratch safety, and bit-identical output.

Relevant files:

- `pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/graph.h`
- `pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/graph.cpp`
- `pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/paramMap.h`
- `pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/nodes/textureNodes.cpp`
- `pxr/imaging/plugin/hdEmbree/renderer/materials/MaterialXCpp/tests/testMaterialXCppGraph.cpp`
- `pxr/imaging/plugin/hdEmbree/OPTIMIZATION.md`

## Root cause

`_nodes` is topologically sorted. Dependencies precede consumers, but unrelated nodes can also precede them.

Connected inputs carry a reevaluation callback. Texture footprints call `ParamMap::Evaluate()` for dx/dy contexts, reaching `_EvaluateNodeOutput()`. It currently executes the full prefix:

```cpp
const size_t nodeCount = static_cast<size_t>(nodeIndex) + 1;
_EvaluateNodes(ctx, nodeCount, scratch);
```

MaterialX implicit-input injection creates a distinct implicit `ND_texcoord_vector2` for every unconnected texture. Reevaluating a later texture's texcoord therefore runs unrelated earlier textures; their footprint probes recursively run more prefixes.

The brick fixture contains six 2048x2048 tiled JPEG nodes. Measurements at 256x256:

| Case | Renderer time |
| --- | ---: |
| Plastic, 1 spp | 0.019 s |
| Brick, 1 spp | 2.491 s |
| Brick, 4 spp | 5.586 s |
| Brick with one explicit shared texcoord, 1 spp | 0.838 s |
| Brick with shared texcoord, 4 spp | 1.031 s |

The shared-texcoord image passed exact `oiiotool --diff`. Perf showed nested `_EvalTextureNode -> _EvaluateNodes -> _EvaluateNodeOutput -> EvaluateInput(texcoord)` stacks. OIIO remains a legitimate secondary cost because six large untiled JPEGs really are sampled.

This bug predates the renderer-layout refactor; the relevant files were renamed without behavioral changes.

## Required invariant

For nested evaluation of node N under context C:

1. Evaluate every transitive upstream dependency exactly once under C.
2. Evaluate N exactly once under C.
3. Preserve compiled topological order.
4. Exclude every unrelated node.
5. Keep absolute node indices and full-indexed scratch storage.
6. Never read output outside the selected closure.
7. Keep outer and active nested scratch objects separate.

## Design

### Store a compiled dependency order

Add to `CompiledNode` in `graph.h`:

```cpp
struct CompiledNode {
    NodeEvalFn evalFn = nullptr;
    std::vector<InputBinding> inputs;
    std::vector<int> reevaluationOrder;
};
```

The order contains the node and all transitive sources, using absolute indices in existing topological order. Worst-case storage is O(N^2), acceptable for current graph sizes. Optimize representation only if production measurements justify it.

### Build orders during Compile()

After all `CompiledNode::inputs` are populated and before `_isValid = true`, compute each target's closure:

```cpp
for (size_t target = 0; target < graph->_nodes.size(); ++target) {
    std::vector<bool> required(graph->_nodes.size(), false);

    std::function<void(int)> mark = [&](int index) {
        if (index < 0 ||
            static_cast<size_t>(index) >= graph->_nodes.size() ||
            required[index]) {
            return;
        }
        required[index] = true;
        for (const InputBinding& input : graph->_nodes[index].inputs) {
            if (input.isConnected) {
                mark(input.sourceNodeIndex);
            }
        }
    };

    mark(static_cast<int>(target));
    auto& order = graph->_nodes[target].reevaluationOrder;
    for (size_t index = 0; index <= target; ++index) {
        if (required[index]) {
            order.push_back(static_cast<int>(index));
        }
    }
}
```

Compilation already rejects cycles. Assert or report an internal compile error if a connected source is invalid or not earlier than its consumer. A non-recursive or incremental implementation is fine if it remains clear and deterministic.

### Evaluate explicit absolute indices

Add:

```cpp
void _EvaluateNodeOrder(
    const ShadingContext& ctx,
    const std::vector<int>& order,
    EvalScratch* scratch) const;
```

Implementation outline:

```cpp
if (scratch->nodeOutputs.size() < _nodes.size())
    scratch->nodeOutputs.resize(_nodes.size());
if (scratch->nodeInputs.size() < _nodes.size())
    scratch->nodeInputs.resize(_nodes.size());

for (int index : order) {
    const CompiledNode& node = _nodes[index];
    if (!node.evalFn) continue;

    ParamMap& inputs = scratch->nodeInputs[index];
    _BuildParamMap(node.inputs, scratch->nodeOutputs, &inputs);

    NodeOutputMap& outputs = scratch->nodeOutputs[index];
    outputs.Clear();
    node.evalFn(inputs, ctx, &outputs);
}
```

Scratch arrays must cover the full graph because bindings use absolute indices. Do not compact or remap. Outputs outside the order may be stale, but no selected node may reference them if closure construction is correct. Always clear selected outputs.

Keep the existing tight full-graph `_EvaluateNodes(ctx, count, scratch)` unless benchmarks show routing full evaluation through an index vector is free. Avoid adding an indirect lookup to every ordinary graph evaluation unnecessarily.

### Change _EvaluateNodeOutput()

Retain validation and the current thread-local, depth-indexed nested scratch pool. Replace only prefix selection:

```cpp
const CompiledNode& target = _nodes[nodeIndex];
_EvaluateNodeOrder(ctx, target.reevaluationOrder, scratch);
```

Fetch `scratch->nodeOutputs[nodeIndex]` as today.

Do not reuse outer scratch: downstream outer nodes still reference it. Do not share one scratch between active recursive depths. A procedural texcoord's real ancestors must still reevaluate under shifted contexts; only unrelated branches disappear.

## Tests

Implement in `testMaterialXCppGraph.cpp`.

### Preserve shifted context

Keep `TestInputReevaluationUsesModifiedContext()` passing unchanged. It must still return roughness 0.75 from U=0.25 shifted by 0.5.

### Exclude unrelated earlier nodes

Register a custom counting evaluator. Build two terminal-input branches:

- an unrelated counting branch deliberately compiled first;
- texcoord -> extract-U -> existing `_EvalOffsetReevaluate` branch.

After one outer evaluation, assert the shifted result is correct and the unrelated counter is exactly one. Current prefix code must fail with a count greater than one. Prove that before applying the fix. Ensure naming/input ordering deterministically puts the unrelated branch first; do not rely on unspecified ordering.

### Deduplicate real dependencies

Use counters on a diamond graph with a shared ancestor. Trigger nested reevaluation and assert every required node runs once in the outer pass and once in the nested pass, while the shared ancestor runs only once per pass.

### Preserve recursive scratch safety

Create two reevaluating nodes in a genuine dependency chain. Verify result and counts, exercising more than one active `nestedScratchDepth`.

### Direct texture regression

Prefer also building several independent image/tiledimage nodes with implicit texcoords and a fake counting `TextureSystem`. Assert each texture receives only required sample calls, with none caused by another texture's footprint reevaluation. Avoid OIIO and filesystem dependencies.

After tests are complete, repository instructions require an adversarial review agent. Ask whether tests fail on old code, guarantee ordering, distinguish outer/nested calls, cover diamonds, and assume valid behavior. Collate findings and discuss before fixing if the workflow is review-only.

## Validation

Focused tests:

```sh
pixi run cmake --build build --target testMaterialXCpp
pixi run ctest --test-dir build -R testMaterialXCpp --output-on-failure
pixi run ctest --test-dir build -R 'testMaterialXCpp|testHdEmbree' --output-on-failure
```

Build/install the plugin before render benchmarks:

```sh
pixi run cmake --build build --target hdEmbree
pixi run build
```

Benchmark `/home/anders/code/typhoon-tests/material-fidelity/surfaces/standard_surface/brick_procedural.usda` through temporary 1 spp and 4 spp stronger layers in `/tmp`. Fix seed, resolution, bounce count, adaptive settings, and renderer settings. Use several repetitions for final numbers.

Expected: the original fixture approaches explicit-shared-texcoord timing, with substantial repeatable improvement over 5.586 s at 4 spp. It remains slower than plastic because six real OIIO samples remain.

Render identical fixed-seed before/after images and require:

```sh
oiiotool --diff before.exr after.exr
```

Also validate a real connected procedural texcoord, bump, or height-to-normal case. Perf should no longer show unrelated texture nodes beneath texcoord reevaluation.

## Documentation

Record root cause, before/after timings, design, image-diff result, and remaining OIIO cost in `OPTIMIZATION.md`. Update `ARCHITECTURE.md` if it describes graph evaluation; add the dependency-only invariant to `AGENTS.md` if it is useful durable maintenance knowledge. Update `README.md` only for user-visible behavior. Follow the local AGENTS requirement to keep affected documentation synchronized.

## Risks

- Missing a dependency can read stale reused-scratch output.
- Membership without topological ordering is incorrect.
- Diamond ancestors must be deduplicated.
- Absolute bindings require full-indexed scratch.
- Nested callbacks require separate scratch per active depth.
- Preserve cycle and unknown-evaluator behavior.
- This does not add visibility-only terminal pruning.
- Do not mix with OIIO changes, constant folding, or implicit-node deduplication.
- Verify benchmarks use the newly installed plugin, not a stale Pixi artifact.

## Implementation sequence

1. Add unrelated-node regression and prove old code fails.
2. Add compiled dependency-order storage.
3. Build and validate transitive orders during compilation.
4. Add absolute-index subset evaluation.
5. Switch `_EvaluateNodeOutput()` to dependency order.
6. Add diamond and nested-depth coverage.
7. Run focused tests.
8. Launch required adversarial test reviewer; collate findings.
9. Build/install hdEmbree.
10. Benchmark brick at 1 and 4 spp with fixed seed.
11. Require exact before/after image match.
12. Validate procedural texcoord/bump/height-to-normal.
13. Confirm profile stacks.
14. Update applicable documentation.
15. Review final diff for unrelated/generated artifacts.

## Completion criteria

- Old prefix code fails the unrelated-node test.
- New code excludes unrelated nodes.
- Shifted contexts remain correct.
- Transitive and diamond dependencies execute exactly once per nested pass.
- Recursive scratch behavior remains safe.
- Focused and relevant hdEmbree tests pass.
- Brick speedup is substantial and repeatable.
- Fixed-seed output is bit-identical.
- Real derivative-driven materials remain correct.
- Perf no longer shows unrelated texture recursion.
- Applicable documentation is updated.
