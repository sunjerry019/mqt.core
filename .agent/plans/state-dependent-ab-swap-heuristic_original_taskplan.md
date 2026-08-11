# Plan: Opt-in stateful A/B swap heuristic

Implement a new opt-in routing mode in the MLIR mapping pass where each
logical/circuit qubit carries a type label A or B that is tracked through swaps.
The search heuristic should use the current state of the routed layout, and each
candidate swap should contribute a type-dependent cost: A<>A = 1, A<>B = 2,
B<>B = 3. The initial A/B assignment is passed into the mapper as an option, and
the mapper must update that assignment whenever a SWAP changes the logical-qubit
placement. Generally, "A" corresponds to an auxiliary qubit, and "B" corresponds
to a data qubit.

## Steps

1. Confirm the routing architecture and add the new opt-in surface in the pass
   options, *depends on the current MappingPass options model*.
2. Extend the MLIR mapping state so the search node/layout tracks logical-qubit
   type labels alongside the current placement, and make swaps update both
   placement and type state together, *depends on step 1*.
3. Rework the heuristic-search cost computation in the mapping pass so swap
   ranking uses the stateful A/B cost model instead of the current
   distance-based heuristic, *depends on step 2*.
4. Plumb the initial A/B mapping from the pass entry point into the
   search/refinement pipeline, and ensure the refined layout carries the updated
   labels through all iterations, *depends on steps 1-3*.
5. Add focused regression tests that prove different A/B states lead to
   different routing choices and that swaps preserve/permute the A/B labels
   correctly, *depends on step 3*.
6. Update the pass documentation to describe the opt-in mode and the new cost
   model, *depends on step 1 and step 3*.

## Relevant files

- `/Users/yudong/Documents/projects/mqt.core/mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`
  — owns `Node`, `search`, `refineLayout`, `route`, and the current A*
  heuristic; this is the primary reimplementation site.
- `/Users/yudong/Documents/projects/mqt.core/mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`
  — documents `MappingPass` options and the current cost model; likely needs a
  new option description.
- `/Users/yudong/Documents/projects/mqt.core/mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h`
  — pass factory surface if the new option requires API threading or helper
  types.
- `/Users/yudong/Documents/projects/mqt.core/mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`
  — best place for end-to-end routing regressions; extend `Sabre`-style coverage
  or add a targeted test for the new heuristic.
- `/Users/yudong/Documents/projects/mqt.core/mlir/unittests/Dialect/QCO/Transforms/Mapping/`
  — test fixture area if a smaller helper test is needed for state transitions
  or swap-cost selection.

## Verification

1. Run the focused mapping unit tests in
   `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` after the
   edit, especially the heuristic-sensitive scenario.
2. Run the narrow build/test target for the MLIR mapping unittest binary, then
   confirm the mapped circuit is still executable on the square-grid device.
3. Run `uvx nox -s lint` after the change batch, per repository policy.

## Decisions

- The change should be opt-in, not a replacement for the existing routing
  heuristic.
- The A/B assignment is provided externally as a mapper option and then tracked
  internally after swaps.
- The implementation belongs in the MLIR mapping pass, not the non-MLIR
  `CircuitOptimizer` path, because the requested behavior concerns
  compilation-time heuristic routing.

## Further Considerations

1. If the initial A/B labels are large or sparse, decide whether to store them
   as a dense per-program-qubit vector or a map keyed by logical qubit index;
   dense storage is likely simpler and faster for search.
2. If you want reproducible behavior across runs, define whether equal-cost
   frontier ties should remain stable or be broken by a secondary rule such as
   depth or swap index ordering.
3. If the new mode should coexist with the old heuristic in the same pass
   binary, add a dedicated option name and keep the current defaults unchanged.
