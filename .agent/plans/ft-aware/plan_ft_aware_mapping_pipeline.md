# Larger plan: FT-aware Rydberg-ion mapping pipeline

Status: brainstorm, pending review — not an ExecPlan yet (see `.agent/PLANS.md`
for that convention) and nothing here is implemented. Scope: the Bacon-Shor /
Rydberg-ion mapping work in `mlir/unittests/Dialect/QCO/Transforms/RydbergIons/`
and `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`. The concrete options
for Phase 1 live in the sibling file `subplan_nn_nnn_cost_encoding.md`, and are
still undecided.

## Motivation

Today, deciding whether an inserted SWAP must be executed fault-tolerantly (FT)
— and therefore blows up into 3 (NN) or 9 (NNN, typical case) physical swaps —
is done **after** mapping, by hand: run the mapper, dump the routed program,
feed it to an external Python state-vector simulator, and manually downgrade
swaps that don't need FT. The mapper itself has no notion of this cost
difference, so it can't route around it.

## Confirmed facts (see memory: project-rydberg-ft-swap-cost-model)

- Target: 12-qubit chain with 3 skip-one edges (`2<>4`, `5<>7`, `8<>10`) forming
  triangular native-gate zones. Chain edges are physical nearest-neighbour (NN);
  skip-one edges are physical next-nearest-neighbour (NNN), despite both being
  direct `CompilerTarget` couplings.
- Physical swap cost table (in units of physical swap operations):

  |          | non-FT (bare) | FT                                     |
  | -------- | ------------- | -------------------------------------- |
  | NN edge  | 1             | 3                                      |
  | NNN edge | 1             | 9 (typical; sometimes an overestimate) |

- Whether a given swap *needs* to be FT is program-state-dependent (checked by
  the external simulator), not a static edge property. The existing
  `qubitTypeLabels` A/B heuristic (`AA=1, AB=2, BB=3`) is already an
  expected-value proxy for FT-necessity, not an arbitrary type cost.
- The full predicted-cost table is multiplicative:
  `cost(edge, labelPair) = typedSwapCost(labelPair) x hopMultiplier(edge)`, with
  `hopMultiplier(NN) = 1`, `hopMultiplier(NNN) = 3`.

**Correction vs. earlier discussion in this thread:** an earlier assumption that
FT-necessity "only matters cost-wise on the 3 NNN edges" was wrong — it was
based on a bad guess (that NN swaps get FT for free) that got corrected.
FT-vs-bare status matters on *every* edge (1 vs 3 on NN, 1 vs 9 on NNN), so any
future feedback/re-routing loop (Phase 3 below) needs to consider all inserted
swaps, not just the 3 NNN ones.

## Phases

### Phase 1 — encode NN/NNN edge cost into the mapping pass's cost model (NOW)

Make the A* search's cost function reflect the multiplicative table above, so
routing decisions are biased toward the topology's real physical cost instead of
treating every coupling edge as equally cheap. This is the current focus; see
`subplan_nn_nnn_cost_encoding.md` for concrete implementation options (still
undecided as of this writing).

### Phase 2 — automate the existing manual one-way pipeline

Turn "run the mapper by hand, dump, simulate, manually downgrade" into a script:

1. Route once (using the Phase 1 cost model as a bias).
2. Dump the routed program (site IDs + swap sequence — already possible via the
   existing `dumpRoutedProgram` debug output).
3. Feed the dump to the external Python simulator; collect a per-swap
   FT-required verdict for **every** inserted swap (not just NNN-edge ones — see
   correction above).
4. A small, separate post-processing/lowering step (outside the mapping pass)
   expands each swap per its verdict and edge: bare swaps stay a single op; FT
   swaps on an NN edge expand to 3; FT swaps on an NNN edge expand to 3
   FT-NN-swaps (which each further expand to 3 — 9 total).

This closes today's manual loop without changing the mapper's search at all
beyond Phase 1, and is a prerequisite for Phase 3 regardless.

### Phase 3 — optional feedback loop (stretch goal, not for the first PoC)

Route -> simulate -> penalize the *specific instances* of swaps the simulator
flagged as FT-required -> re-route from scratch -> re-simulate -> repeat (capped
iterations, keep the best-by-final-swap-count result). Open problems: matching a
simulator verdict from one routing to the right swap occurrence in a completely
different re-routing has no clean canonical key; there's no convergence
guarantee (fixing one crossing can relocate the problem); and the "9" cost
itself is noted to be an occasional overestimate, adding noise to whatever
signal drives re-routing. Worth revisiting only once Phase 2 exists and its
real-vs-predicted swap counts have been measured.

### Phase 4 — initial-layout ordering heuristic (independent, optional)

User's manual observation: seeding the initial layout in program/gate order
(rather than randomly) tends to produce fewer swaps. Checked the current
`generateLayout` (Mapping.cpp:953-1003): it only ever uses `Layout::random(...)`
for all `ntrials` trials, then SABRE-style forward/backward refinement
(`niterations` passes) picks the best. There is **no existing mechanism** to
seed a trial from program/gate order — this would be new code (e.g. add one
deterministic, order-derived candidate layout alongside the random restarts).
Purely an optimization idea for later, not validated as ground truth by the
user, independent of the FT-cost-model work.

### Phase 5 — Python integration (explicitly deferred; brainstorm for another time)

The simulator lives in a separate Python project, not this repository. Today the
only integration point is file-based (dump -> hand off -> read verdicts back).
If/when Python bindings for this MLIR mapper exist (or get added — e.g. exposing
the mapping pass as a callable via nanobind/pybind11, similar to bindings
elsewhere in the mqt-core stack), that would let the route -> simulate ->
reroute loop in Phase 3 run in-process instead of via file dump/reload, which is
what would actually make Phase 3 practical. Not to be designed now — flagged
only, per explicit instruction.

## Open questions / risks

- Should `h(n)` (the A* lookahead heuristic) also become edge-weight-aware, or
  is it fine to leave it hop-count-based given the search isn't provably optimal
  anyway? (Leaning: leave it, revisit only if Phase 2 measurements show the
  heuristic is misleading the search badly.)
- Where the NN/NNN edge-cost data should live (pass-local vs. on
  `CompilerTarget` itself) — this is the crux of the Phase 1 subplan, still
  undecided.
- How much the "9 is sometimes an overestimate" noise actually matters for Phase
  1's bias quality can only be evaluated empirically once Phase 2's automation
  exists to compare predicted vs. actual final swap counts.
