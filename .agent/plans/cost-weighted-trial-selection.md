# Select the mapping pass's best initial layout by routing cost rather than raw SWAP count

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

## Purpose / Big Picture

Before the mapping pass routes a circuit for real, it tries several random
starting assignments of program qubits to hardware sites, refines each one, and
keeps the best. "Best" is currently defined as the assignment that produced the
fewest SWAP operations. But when the user has opted into either of the pass's
two cost heuristics, the search that produced those SWAPs was not minimising
their count; it was minimising a weighted cost in which different SWAPs are
worth different amounts. The selection step therefore ranks candidates by a
different yardstick than the one the search optimised, and can discard the
candidate the cost model actually prefers.

After this change, when a cost heuristic is active the pass selects the
candidate with the lowest total weighted cost, and the two steps agree. The
user-visible outcome is that increasing the number of trials now reliably
improves the metric the user configured, instead of improving a proxy that can
point the other way.

You can see it working by routing a circuit with many trials and a cost
heuristic enabled, before and after the change, and comparing the new total-cost
statistic this plan adds: it must not increase, and on inputs where the two
metrics disagree it will decrease.

## Progress

- [ ] Read the orientation section and confirm the named code matches the
      current working tree.
- [ ] Extend the statistics structure and thread the goal node's cost out of the
      search.
- [ ] Change the trial selection comparison.
- [ ] Add the total-cost statistic to the pass declaration and accumulate it.
- [ ] Add the GoogleTest described under Validation and Acceptance.
- [ ] Build, run the mapping tests, and record results in
      `Outcomes & Retrospective`.

## Surprises & Discoveries

- Observation: the quantity the selection step compares is not even the SWAP
  count of the routing that will finally be emitted. Evidence: `generateLayout`
  scores each trial by the SWAP count of a preliminary backward refinement pass
  that emits no instructions, after which the chosen layout is routed once more
  for real by a separate call. The score is therefore already a proxy; this plan
  makes it a proxy for the right thing.

## Decision Log

- Decision: compare by weighted cost only when a cost heuristic is active, and
  keep comparing by SWAP count otherwise. Rationale: when neither heuristic is
  enabled the pass defines every SWAP as costing one, so the two quantities are
  identical and there is no behaviour to change; restricting the change keeps
  every existing test on the default path valid. Date/Author: 2026-09-18, this
  plan.

- Decision: make this plan a prerequisite of, rather than an alternative to, the
  user-supplied initial layout described in
  `.agent/plans/initial-layout-option.md`. Rationale: that plan lets a user
  offer a layout they believe is good, and in its default mode that layout
  competes against randomly generated ones under exactly the comparison this
  plan fixes. A genuinely good supplied layout can therefore lose to a worse
  random one whenever the two metrics disagree, which is the same defect this
  plan addresses and is more visible there because the user has an expectation
  about the outcome. Date/Author: 2026-09-18, this plan.

- Decision: obtain the cost by returning the chosen goal node's accumulated cost
  out of the A* search, rather than recomputing each SWAP's cost when the SWAPs
  are inserted. Rationale: the search already computes exactly this number in
  order to rank its own frontier, so returning it is free and cannot disagree
  with what the search believed, whereas recomputing invites the two to drift
  apart. Date/Author: 2026-09-18, this plan.

## Outcomes & Retrospective

Not yet started. On completion, record the SWAP count and total cost observed
for a representative circuit before and after the change, at several trial
counts.

## Context and Orientation

Everything below is a fact about the repository as it stands. Verify it.

Terms, in plain language. The *mapping pass* inserts SWAP operations into a
quantum circuit so that every multi-qubit gate acts on hardware sites that are
directly connected. It is declared in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` as `def MappingPass`,
command-line name `place-and-route`, and implemented as the single `MappingPass`
struct in `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`. A *layout* is
an assignment of program qubits to hardware sites. *Routing* is the insertion of
SWAPs. *A\** is the search algorithm the pass uses to find a short SWAP
sequence.

The pass has two opt-in cost heuristics. The first is enabled by the
`qubitTypeLabels` option, a string with one character per program qubit where
`A` marks an auxiliary qubit and `B` a data qubit; when set, a SWAP costs one,
two or three depending on whether it exchanges two auxiliary qubits, one of
each, or two data qubits. The second is enabled by the `nnnEdges` option, a
comma-separated list of site pairs such as `2-4,5-7,8-10`; when set, a SWAP
crossing one of those edges has its cost multiplied by `nnnCostMultiplier`,
default three. Both default to empty, meaning off, in which case every SWAP
costs one.

The relevant code, with approximate current line numbers. `struct Statistics` at
line 160 contains one field, `size_t nswaps{0};`. The per-SWAP cost is
accumulated into a `Node`'s `pathCost` field in the `Node` constructor at
approximately lines 215 to 230. `Node::g` at approximately line 259 returns
`alpha * pathCost` when either heuristic is active and `alpha * depth`
otherwise. The A* search is `search()` at approximately line 1091; it returns
the sequence of SWAPs leading to the goal node, reconstructed by walking parent
pointers from the goal at approximately line 1141. `insertSWAPs` at
approximately line 1394 records `stats.nswaps += swaps.size();` at approximately
line 1431.

The trial loop is `generateLayout` at approximately lines 1029 to 1079. It seeds
a random number generator from the `seed` option, builds `ntrials` candidate
layouts with `Layout::random`, refines each in parallel with `parallelForEach`
by running `niterations` forward-and-backward routing passes, and then selects,
as indented source:

    Trial* best = nullptr;
    for (Trial& t : trials) {
      if (t.success &&
          (best == nullptr || best->stats.nswaps > t.stats.nswaps)) {
        best = &t;
      }
    }

Note that the loop resets `t.stats.nswaps = 0;` before the backward pass, so the
score reflects only the final backward refinement.

After `generateLayout` returns a layout, `runOnOperation` at approximately lines
466 to 476 calls `place`, then performs one real routing pass, then records
`numSwaps += stats.nswaps;` into the pass's `numSwaps` statistic, declared in
the tablegen file as `Statistic<"numSwaps", "num-inserted-swaps", ...>`.

One caveat that affects how you validate this change: the pass does not produce
identical output across repeated runs of the same binary with the same inputs
and seed, because `mlir/include/mlir/Dialect/QCO/Utils/Drivers.h` declares a
hash map keyed on raw pointer values whose iteration order varies between
process launches. Do not write a test that asserts an exact SWAP sequence.

## Plan of Work

In `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`, add a second field to
`struct Statistics` beside `nswaps`, namely `float cost{0.0F};`, documented as
the total weighted cost of the inserted SWAPs, equal to the count when no cost
heuristic is active.

Change `search()` so that in addition to the SWAP sequence it reports the
accumulated cost of the goal node it selected. The least invasive way is to give
it an extra out-parameter, a `float&` the caller supplies, assigned from the
goal node's `pathCost` at the point where the SWAP sequence is reconstructed.
Keep the return type as it is so that the many other call sites are unaffected,
and give the parameter a default so that callers that do not care need not
change.

In `insertSWAPs`, beside the existing `stats.nswaps += swaps.size();`, add
`stats.cost += <the cost reported by the search for this sequence>;`. If the
call structure makes it awkward to pass the cost down into `insertSWAPs`,
accumulate it in the caller instead; what matters is that `stats.cost` ends up
holding the sum over the whole routing pass, and that it is reset wherever
`stats.nswaps` is reset.

Note for whoever implements this after `.agent/plans/initial-layout-option.md`:
that plan makes the first trial carry a user-supplied layout rather than a
random one when the `initialLayout` option is set. Nothing here needs to change
for that to work, because the seeded trial is refined and scored like any other;
but the new comparison is what decides whether the user's layout is kept, so
make sure the two plans are tested together at least once, with a supplied
layout and `ntrials` greater than one.

In `generateLayout`, change the selection comparison to compare `stats.cost`
when either cost heuristic is active and `stats.nswaps` otherwise. Express the
condition using the same flags the `Node` constructor already uses to decide
whether to apply typed or edge costs, so that the two cannot disagree about
whether a heuristic is on. Preserve the existing tie-breaking behaviour, which
is that a strict greater-than comparison keeps the earliest trial among equals;
this matters because it is the only reason the selection is deterministic at
all.

In `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`, add a statistic beside
the existing `numSwaps` one, named `totalSwapCost` with command-line name
`total-swap-cost-milli`, documented as one thousand times the total weighted
cost of the inserted SWAPs. The scaling to an integer is required because MLIR
statistics are integers. In `runOnOperation`, accumulate it next to the existing
`numSwaps += stats.nswaps;`.

## Concrete Steps

Work from the repository root at `/Users/yudong/Documents/projects/mqt.core`.

Establish a baseline before editing:

    cmake --preset release
    cmake --build --preset release --target mqt-core-mlir-unittest-mapping
    ./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping

Expect a transcript ending in a line reporting all tests passed; record the
count.

After the edits, rebuild and rerun the same binary. The count must be identical,
because every existing test either leaves both heuristics off, in which case the
comparison is unchanged by construction, or sets `ntrials` to one, in which case
there is nothing to compare.

To observe the statistic, build and run the Rydberg-ion routing tool if the
ExecPlan `.agent/plans/rydberg-ion-routing-tool.md` has been completed, passing
the MLIR pass-statistics flag. Otherwise run the Rydberg-ion unit-test binary,
which exercises a configuration with both heuristics enabled.

## Validation and Acceptance

Add one GoogleTest to
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`, beside the
existing tests for the two cost heuristics.

The test must demonstrate that selection now follows cost. Construct a target
and a small circuit, and choose `qubitTypeLabels` so that some SWAPs are much
more expensive than others. Run the pass twice with an identical seed and an
identical trial count greater than one, once with the labels set and once
without. Assert that with labels set, the reported `total-swap-cost-milli`
statistic is less than or equal to the cost of the layout that the SWAP-count
rule would have chosen.

Constructing that assertion directly requires knowing what the old rule would
have chosen, which the pass no longer reports. The practical form is therefore:
instrument the test to route with `ntrials` equal to one for each of several
seeds, record each single-trial cost, then route with `ntrials` equal to the
number of seeds and the same seed sequence, and assert that the multi-trial cost
is less than or equal to the minimum of the single-trial costs. That is the
property the selection is supposed to guarantee and it is the property that was
being violated.

Because routing is not reproducible across process launches, run this assertion
within a single process, which the test naturally does, and do not assert exact
equality against a hardcoded number.

The second acceptance criterion is that the default path is untouched: the full
mapping test binary reports the same number of passing tests as the recorded
baseline.

## Idempotence and Recovery

All steps are repeatable and nothing outside the build directory is modified.

The change is small but touches a hot path shared by every target, so the
recovery path if the mapping tests regress is to confirm that the new comparison
falls back to `nswaps` whenever both heuristics are off, which is the condition
under which all pre-existing tests run. If `stats.cost` is ever left
uninitialised or un-reset, the symptom will be a trial selection that looks
random; check that `cost` is reset everywhere `nswaps` is reset, including the
explicit `t.stats.nswaps = 0;` inside the refinement loop.

## Artifacts and Notes

The mismatch this plan fixes, stated precisely: `Node::g` returns
`alpha * pathCost` when a heuristic is active, so the search minimises
`pathCost`; but `generateLayout` selects on `stats.nswaps`, which is incremented
by `swaps.size()` and therefore counts SWAPs with equal weight regardless of the
labels or edges involved. With the A/B heuristic active a SWAP can cost three
times another, and with the edge heuristic active another factor of three, so
the two quantities can differ by up to ninefold for the same routing.

## Interfaces and Dependencies

No new libraries. The change is confined to `MLIRQCOTransforms` and its tablegen
declaration.

At the end of this milestone the following must exist. `struct Statistics` in
`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` must carry both a SWAP
count and a floating-point total cost. `def MappingPass` in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` must declare a statistic
named `totalSwapCost` with command-line name `total-swap-cost-milli` alongside
the existing `numSwaps`. No public function signature outside the pass may
change, and in particular `createMappingPass` and the generated
`MappingPassOptions` struct must be unaffected, so that
`mlir/lib/Compiler/TargetCompilation.cpp` continues to compile unchanged.
