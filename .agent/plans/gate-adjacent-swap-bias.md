# Bias the mapping pass toward SWAPs that immediately follow a gate on the same qubit pair

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

**This plan must not be started until
`.agent/plans/gate-adjacent-swap-statistic.md` has been implemented and its
measurement recorded.** That plan adds the counting machinery and the statistic
this plan depends on, and it produces the baseline number without which this
plan's effect cannot be judged. If that measurement showed the opportunity to be
negligible, read `Decision Log` below before proceeding: abandoning this plan is
an acceptable outcome.

## Purpose / Big Picture

When the compiler inserts a SWAP operation, that SWAP may later have to be built
in an expensive protected form, which on this hardware costs roughly seven and a
half times what the cheap unprotected form costs. Whether a given SWAP needs
that protection is decided outside the compiler, by a simulator.

There is one situation where it is known in advance that the protection is
unnecessary: when the SWAP acts on exactly the pair of qubits that a two-qubit
gate of the original circuit has just acted on, with nothing in between touching
either qubit. The companion plan named above added a statistic counting how
often that happens. This plan makes the router *prefer* it, by telling the
search that such a SWAP is cheaper than it otherwise looks.

After this change, a user can supply a discount factor, and the search will
favour placements that produce these cheap SWAPs. The user-visible outcome is a
routed circuit whose total expected error is lower, because a larger fraction of
its SWAPs can be left unprotected.

You can see it working by routing the same circuit across many seeds with and
without the discount and comparing the distribution of the
`num-gate-adjacent-swaps` statistic: its mean must rise, and the mean total SWAP
cost must not rise with it.

## Progress

- [ ] Confirm `.agent/plans/gate-adjacent-swap-statistic.md` is implemented and
      that its recorded baseline justifies continuing.
- [ ] Read the orientation section and confirm the named code matches the
      current working tree, in particular the claim that the search early-exits
      when the front layer is already executable.
- [ ] Add the pass option.
- [ ] Thread the qualifying-pair set into the search node cost.
- [ ] Add the two GoogleTests described under Validation and Acceptance.
- [ ] Measure the effect over at least thirty seeds at several discount values
      and record it in `Outcomes & Retrospective`, including the case where it
      does not help.

## Surprises & Discoveries

- Observation: only one of the two possible orderings can ever occur, which
  greatly simplifies the implementation. A SWAP cannot be immediately followed
  by a two-qubit gate on the same site pair. If after a SWAP on sites a and b
  the gate's two qubits occupy a and b, then before the SWAP they occupied b and
  a, the same unordered pair, so they were already adjacent and the gate was
  already executable; the search returns an empty SWAP sequence in that case
  without emitting anything. Evidence: scanning four routed Bacon-Shor circuits
  found zero occurrences of a SWAP followed by a gate on the same pair, and
  five, two, two and two occurrences of the reverse ordering. Only the
  gate-then-SWAP ordering therefore needs to be handled, and only for the first
  SWAP of each search, because no gates execute during a search.

- Observation: the opportunity is rare in the circuits measured so far, so the
  value of this change is unproven. Evidence: the same scan found between two
  and five qualifying SWAPs out of seventy-six to eighty-eight total. Whether
  biasing the search increases that number materially is exactly what this
  plan's measurement step exists to find out, and a negative result must be
  recorded rather than tuned away.

## Decision Log

- Decision: implement this as a cost discount inside the search rather than as a
  peephole rewrite applied after routing. Rationale: the goal is to change which
  placements the router chooses, and a rewrite applied afterwards cannot do
  that; it can only clean up placements that were already chosen for other
  reasons. Date/Author: 2026-09-18, this plan.

- Decision: apply the discount only to the first SWAP of each search invocation.
  Rationale: the qualifying condition refers to a gate that has already
  executed, and no gates execute during a search, so no SWAP after the first can
  qualify; restricting to the first also keeps the cost a function of the search
  node alone, which the search's duplicate-state pruning requires. Date/Author:
  2026-09-18, this plan.

- Decision: express the option as a multiplicative discount on the SWAP's cost
  rather than as a boolean. Rationale: the correct discount depends on which
  cost model is active and on hardware constants that are still being measured,
  so a float lets the user calibrate it without another code change, and a value
  of one disables the feature entirely. Date/Author: 2026-09-18, this plan.

- Decision: split the measurement out of this plan into
  `.agent/plans/gate-adjacent-swap-statistic.md` and make this plan depend on
  it. Rationale: shipping the instrument together with the change it is meant to
  evaluate makes the baseline unobservable, and this plan's payoff is genuinely
  uncertain, so the baseline is the deciding evidence. Date/Author: 2026-09-18,
  this plan.

- Decision: abandoning this plan is an acceptable outcome. Rationale: if the
  baseline measurement shows only a handful of qualifying SWAPs per routing and
  the discount does not move that number, the change adds an option and a branch
  on the search's hot path for no benefit, and the honest response is to record
  that and stop. Whoever reaches that point must write the finding into
  `Outcomes & Retrospective` rather than deleting the plan. Date/Author:
  2026-09-18, this plan.

## Outcomes & Retrospective

Not yet started. On completion, record the distribution of the
`num-gate-adjacent-swaps` statistic and of the total SWAP cost across at least
thirty seeds, with the discount disabled and at each discount value tried, and
state plainly whether the bias increased the count and whether total cost fell.

## Context and Orientation

Everything below is a fact about the repository as it stands. Verify it.

Terms, in plain language. The *mapping pass* inserts SWAP operations into a
quantum circuit so every multi-qubit gate acts on directly connected hardware
sites. It is declared in `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` as
`def MappingPass`, command-line name `place-and-route`, implemented as the
single `MappingPass` struct in
`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`. A *site* is a hardware
qubit position. A *program qubit* is a qubit of the user's circuit; which site
holds which program qubit changes as SWAPs execute, and that assignment is the
*layout*. A *fault-tolerant SWAP* is an expensive protected realisation; a
*bare SWAP* is the cheap unprotected one. The pass never decides which is used;
it only weighs how likely the expensive form is.

The routing loop works as follows. `advance()`, at approximately line 1438,
executes every gate whose qubits are already on directly connected sites, and
stops when no more can be executed. `getWindow()`, at approximately line 1343,
then collects the gates that are blocked, in layers. `search()`, at
approximately line 1091, runs an A\* search for a sequence of SWAPs that makes
the first layer executable. `insertSWAPs`, at approximately line 1394, writes
those SWAPs into the circuit. The loop then returns to `advance()`. Crucially,
no gates execute between the start and the end of one `search()` call.

The search expands a node by trying every coupling edge incident to a qubit in
the front layer, at approximately lines 1155 to 1170. It early-exits before
expanding anything if the front layer is already executable, at approximately
lines 1104 to 1109; this is the fact that makes the SWAP-then-gate ordering
impossible.

Each search node accumulates a `pathCost` in its constructor at approximately
lines 215 to 230. Today that accumulation is, as indented source:

    float base = 1.0F;
    if (useTypedCost) { ... base = typedSwapCost(qubitLabels[prog0], qubitLabels[prog1]); }
    const float edgeMultiplier =
        (useEdgeCost && nnnEdges.contains(swap)) ? nnnCostMultiplier : 1.0F;
    pathCost += base * edgeMultiplier;

`Node::g`, at approximately line 259, returns `alpha * pathCost` when a cost
heuristic is active and `alpha * depth` otherwise. A node's `depth` field
records how many SWAPs lie between it and the root, so the first SWAP of a
search is the one on a node of depth one.

The search prunes a node if it has already seen the same layout at an equal or
lower depth, at approximately lines 1113 and 1126 to 1136, using a map keyed on
the layout. This is why a cost term must be a function of the node itself and
not of how the node was reached: a term that depended on the path would make two
nodes with the same layout genuinely different, and the pruning would discard
the cheaper one. A discount applied only at depth one satisfies this, because
depth is part of the node.

The prerequisite plan `.agent/plans/gate-adjacent-swap-statistic.md` has already
added to this file two per-site vectors of `Operation*`, one recording the most
recent two-qubit gate on each site and one recording the most recent gate of any
arity, both updated in `advance()`; a predicate that decides whether a site pair
qualifies; a counter in `struct Statistics`; and the statistic
`numGateAdjacentSwaps`, command-line name `num-gate-adjacent-swaps`. This plan
reuses all of it and adds none of it. If any of those pieces is missing, stop
and implement that plan first.

Two existing pass options establish the pattern this plan follows, both opt-in
and both defaulting to off. `qubitTypeLabels` takes a string with one character
per program qubit, `A` for auxiliary and `B` for data, and makes SWAP cost
depend on the pair of roles. `nnnEdges` takes a comma-separated list of site
pairs such as `2-4,5-7,8-10` and multiplies the cost of SWAPs crossing them by
`nnnCostMultiplier`. Their parsers, `parseQubitLabels` and `parseNnnEdges`, are
at approximately lines 489 and 522 and are called from `runOnOperation` at
approximately lines 411 to 432.

One caveat for validation: the pass does not produce identical output across
repeated runs of the same binary with the same inputs and seed, because
`mlir/include/mlir/Dialect/QCO/Utils/Drivers.h` line 37 declares a hash map
keyed on raw pointer values whose iteration order varies between process
launches. Do not assert an exact SWAP sequence, and do not conclude from a
single pair of runs that the option helped or did not.

For scale, on the Rydberg-ion hardware the expected error of a bare SWAP on a
nearest-neighbour edge is one unit, and of a fault-tolerant SWAP on the same
edge roughly seven and a half units. A discount of about one seventh therefore
represents "this SWAP is certainly bare" under that cost model.

## Plan of Work

In `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`, add one option to the
`let options` list of `def MappingPass`, named `gateAdjacentSwapDiscount`,
command line name `gate-adjacent-swap-discount`, of type `float`, default
`1.0F`, documented as the factor by which the cost of a SWAP is multiplied when
it acts on exactly the site pair that a two-qubit gate has just acted on with
nothing in between touching either site; a value of one, the default, disables
the feature. Extend the pass description in the same file with a prose paragraph
in the style of the two existing heuristic paragraphs, explaining why such a
SWAP is cheaper.

In `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`, compute once per
`search()` call, before the search begins, the set of site pairs that satisfy
the predicate the prerequisite plan added. There is at most one qualifying
partner per site, but several distinct pairs may qualify simultaneously, so
build a small set rather than a single pair. Pass that set to the node
constructor alongside the existing `nnnEdgeSet` and `qubitLabels`.

In the node constructor, after computing `base` and `edgeMultiplier` as today,
multiply the result by `gateAdjacentSwapDiscount` when the node's depth is one
and the node's SWAP is in that set. Guard the whole thing so that when the
option is left at one, the arithmetic is unchanged.

Assert, in a form that survives into debug builds, that the discount is never
applied at a depth other than one. That assertion is the guard against the
duplicate-state pruning hazard described in the orientation section, and it is
cheap.

The statistic itself needs no change: the prerequisite plan already counts
qualifying SWAPs unconditionally, so the same statistic measures the baseline
and the improvement.

## Concrete Steps

Work from the repository root at `/Users/yudong/Documents/projects/mqt.core`.

Establish a baseline:

    cmake --preset release
    cmake --build --preset release --target mqt-core-mlir-unittest-mapping
    ./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping

Record the number of passing tests. After the edits, rebuild and rerun; the
count must be identical, because every existing test leaves the new option at
its default of one.

For the measurement, use the routing tool from
`.agent/plans/rydberg-ion-routing-tool.md`, which is required rather than merely
preferred here: the effect must be measured over at least thirty seeds, and
changing the seed in the unit test requires a rebuild. With that tool built,
collect `num-gate-adjacent-swaps` and the total cost across a range of seeds
with the discount disabled, then repeat with several discount values such as one
half, one quarter and one seventh. Compare distributions, not individual runs.

The harness described in
`/Users/yudong/Documents/projects/qec-rydberg-ions/.agent/plans/compiler-evaluation-harness.md`
automates exactly this sweep and is the better instrument if it exists.

## Validation and Acceptance

Add two GoogleTests to
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`.

The first asserts inertness at the default. Route a small circuit twice with
identical options except that the second sets `gateAdjacentSwapDiscount` to one
explicitly, and assert the routed modules are structurally identical. This
proves the default path is untouched.

The second asserts the bias changes a decision. Construct a target and a circuit
in which, at some point during routing, two candidate first SWAPs are equally
good under the existing cost model, and exactly one of them acts on the site
pair of the gate that has just executed. With the discount at one the choice
between them is arbitrary; with the discount well below one the qualifying SWAP
must win. Assert on the operands of the first emitted SWAP operation, following
the assertion style the existing `qubitTypeLabels` test uses. Construct the
circuit so that the two candidates are genuinely tied under the old model,
otherwise the test proves nothing about the new term.

The behavioural acceptance for the overall purpose is the measurement described
under Concrete Steps: over at least thirty seeds, enabling the discount must
increase the mean of the `num-gate-adjacent-swaps` statistic and must not
increase the mean total SWAP cost. If the measurement shows the qualifying count
barely moves, that is a legitimate outcome and must be recorded in
`Outcomes & Retrospective` together with a recommendation about whether to keep
the option. Do not tune the test circuit until the measurement looks good; the
measurement is the experiment, not the acceptance gate.

## Idempotence and Recovery

All steps are repeatable and nothing outside the build directory is modified.

The risk concentrated in this change is the interaction with the search's
duplicate-state pruning. If the discount is ever applied at a depth other than
one, two nodes with the same layout can carry different costs and the pruning
will discard the cheaper one, producing worse routings in a way that is hard to
notice because the output is still correct. The debug assertion required above
is the guard; keep it.

If the mapping tests regress, confirm first that the option is at its default in
those tests and that the guarded arithmetic reduces exactly to the original
expression.

## Artifacts and Notes

The reasoning behind the rule, recorded so a later reader does not have to
reconstruct it. A SWAP is three CZ gates on this hardware, and a fault-tolerant
SWAP is nine, arranged as three bare SWAPs through an ancilla. Protection is
needed when a single fault on one of the two qubits could propagate onto the
other and produce a correlated error of weight two that the code cannot correct.
If a two-qubit gate of the original circuit has just coupled exactly those two
qubits, that propagation path already existed, and the original circuit was
designed to tolerate it. Hence the SWAP adds no new uncorrectable path and may
stay bare.

A reference implementation of the related circuit rewrite, which is not what
this plan does but is worth reading for the algebra, is
`CircuitOptimizer::cancelCNOTs` in `src/circuit_optimizer/CircuitOptimizer.cpp`
at approximately lines 1182 to 1202, which rewrites a controlled-NOT followed by
a SWAP on the same pair into two controlled-NOTs. It belongs to the repository's
legacy non-MLIR code path and is not wired into this pipeline.

## Interfaces and Dependencies

No new libraries. The change is confined to `MLIRQCOTransforms` and its tablegen
declaration. It depends on `.agent/plans/gate-adjacent-swap-statistic.md` having
landed first.

At the end of this milestone the following must exist. `def MappingPass` in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` must declare an option
`gateAdjacentSwapDiscount` with command-line name `gate-adjacent-swap-discount`
defaulting to one, alongside the statistic `numGateAdjacentSwaps` that the
prerequisite plan added. The generated `MappingPassOptions` struct gains one
correspondingly named field, and the existing caller in
`mlir/lib/Compiler/TargetCompilation.cpp`, which constructs a default
`MappingPassOptions{}`, must continue to compile unchanged.
