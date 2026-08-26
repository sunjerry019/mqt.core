# Encode NN/NNN edge cost into the mapping pass's SWAP cost model

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

## Purpose / Big Picture

`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` implements the
`place-and-route` pass (`MappingPass`), which inserts `qco.swap` operations so
that every two-qubit operation in a quantum program ends up on adjacent sites of
a target device (a `CompilerTarget`, declared in
`mlir/include/mlir/Compiler/Target.h`). It chooses which SWAPs to insert with an
A* search. The pass already supports one opt-in cost refinement: the
`qubit-type-labels` option (see `Node::typedSwapCost` in `Mapping.cpp`) prices
each candidate SWAP by the "A" (auxiliary) or "B" (data) role of the two program
qubits it would exchange (`A<>A=1, A<>B=2, B<>B=3`), instead of a flat per-SWAP
cost of `1`. Independently of qubit role, some hardware targets have
coupling-graph edges that are direct connections in the target's topology
description but are physically more distant, and therefore more expensive to
use, than other edges. Concretely, the Rydberg-ion target built by
`getRydbergIonTarget()` in
`mlir/unittests/Dialect/QCO/Transforms/RydbergIons/test_rydberg_ions.cpp` is a
12-site chain (sites `0` through `11`) with three extra "skip-one" couplings,
`2<>4`, `5<>7`, and `8<>10`, added so that three-qubit gates work inside three
triangular zones. The chain edges (`0<>1`, `1<>2`, `2<>3`, and so on) connect
physically adjacent ions ("nearest neighbour", NN); the three skip-one edges
connect physically next-nearest ions ("next-nearest neighbour", NNN). On the
real hardware, a SWAP that must be executed fault-tolerantly (a stronger,
error-corrected form of the operation, decided by simulation outside this pass
and outside this repository) costs 3 ordinary swap operations on an NN edge, but
typically 9 on an NNN edge, because an NNN fault-tolerant SWAP must be
decomposed into 3 fault-tolerant NN SWAPs. `MappingPass` currently has no way to
know that some of its coupling edges are more expensive than others: every edge
in `target->forEachNeighbour(...)` is treated identically by the A* search.

After this change, a caller of `MappingPass` can opt in to an "NNN edge cost"
heuristic by passing two new pass options together: `nnn-edges` (C++ option
field `nnnEdges`), a string listing which of the target's coupling edges should
be treated as expensive (for example `"2-4,5-7,8-10"`, using the target's own
site identifiers exactly as they appear in the `CompilerTarget`'s coupling
list), and `nnn-cost-multiplier` (C++ option field `nnnCostMultiplier`), a float
(default `3.0`) that says how much more expensive those edges are. Once both are
set, every candidate SWAP the A* search considers on one of the listed edges has
its cost multiplied by `nnn-cost-multiplier` relative to the same SWAP on any
other edge. This composes multiplicatively with the existing `qubit-type-labels`
heuristic: when both options are set, a SWAP's cost is
`typedSwapCost(labels) * (edge is listed in nnn-edges ? nnn-cost-multiplier : 1)`;
when only `nnn-edges` is set, the cost is simply `1 * multiplier` or `1`; when
only `qubit-type-labels` is set, behavior is exactly what it is today. Leaving
`nnn-edges` unset (the default, an empty string) reproduces the exact previous
behavior byte-for-byte, regardless of `nnn-cost-multiplier` or
`qubit-type-labels` — this is a strictly additive, opt-in-only change, and it
must stay that way: it must never change the pass's output for any existing
caller that does not pass the new option.

The observable proof: on a small hand-built topology containing two alternative
routes between the same pair of program qubits, one route using only
NN-classified edges and the other using an edge named in `nnn-edges`, running
the pass with `nnn-edges` naming the more expensive edge and a multiplier
greater than 1 makes the A* search prefer the NN-only route (or, when both
routes tie in length, the route that uses the listed edge fewer times), whereas
running the same program and target with `nnn-edges` unset reproduces today's
flat-cost behavior. A new focused GoogleTest in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` demonstrates
this directly, the same way the existing `StatefulSwap*` tests demonstrate the
`qubit-type-labels` heuristic.

## Progress

- [ ] Not yet started. This ExecPlan was authored from a design discussion and
      has not had any of its `Plan of Work` steps implemented yet.

## Surprises & Discoveries

None yet; this section will be filled in as implementation proceeds.

## Decision Log

- Decision: implement the NN/NNN edge cost as a pass-local, opt-in edge set
  stored on `MappingPass` (referred to as "Option A" during design), rather than
  as a change to `CompilerTarget::Coupling` itself (referred to as "Option B",
  described in full in `Artifacts and Notes` below). Rationale: explicit user
  choice on 2026-08-26, made to keep the diff small (no changes to
  `mlir/include/mlir/Compiler/Target.h`/`Target.cpp` or to any existing code
  that constructs a `CompilerTarget`'s coupling list) and because the fact
  "these specific edges are more expensive" is, for now, only needed by this one
  pass. Option B remains recorded in case this decision is revisited if another
  pass or tool later needs the same physical-edge-cost fact.
- Decision: the new heuristic must default to exactly today's behavior and
  require an explicit opt-in, mirroring how `qubit-type-labels` defaults to an
  empty string that disables its heuristic. Rationale: explicit user instruction
  on 2026-08-26 ("The default behaviour should make sure that existing behaviour
  is maintained, and the option of taking into account NN/NNN ... must be
  explicitly turned on").
- Decision: both the edge list (`nnn-edges`) and the multiplier
  (`nnn-cost-multiplier`) are exposed as ordinary TableGen `Option`s on
  `MappingPass` (CLI-settable, like `alpha`/`lambda`/`qubit-type-labels`), not
  as C++-only constructor parameters and not as hardcoded constants. Rationale:
  explicit user answers during design discussion on 2026-08-26 — the edge list
  "may not always apply to the compilation target" (so it must be something a
  caller can simply omit), and the multiplier should be "tunable but optional...
  just like nlookahead etc."
- Decision: the `qubit-type-labels` heuristic and the new `nnn-edges` heuristic
  are independent and multiply together when both are enabled; neither requires
  the other to be set. Rationale: a caller who only cares about edge distance
  should not have to fabricate qubit-type labels, and vice versa; this also
  matches the confirmed physical cost model
  (`cost(edge, labelPair) = typedSwapCost(labelPair) * hopMultiplier(edge)`,
  where either factor can independently be the neutral value `1`).
- Decision: the A* lookahead heuristic `h(n)` is left unchanged (still the
  unweighted coupling-graph distance sum); only the realized path cost `g(n)`
  becomes edge-cost-aware. Rationale: matches the precedent set when
  `qubit-type-labels` was added (see `Node::h`'s doc comment in `Mapping.cpp`,
  which was deliberately left untouched by that change for the same reason):
  estimating the edge-cost of not-yet-chosen future SWAPs along an unknown
  remaining path is speculative, and the search is not claimed to be provably
  optimal regardless.

## Outcomes & Retrospective

Not yet started; to be completed once implementation is done.

## Context and Orientation

`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` implements `MappingPass`,
an MLIR pass (`mlir::qco::MappingPass`, registered as pass `place-and-route`)
that lowers dynamically allocated ("program") qubits onto the static sites of a
`CompilerTarget` and inserts `qco.swap` operations (`SWAPOp`, declared in
`mlir/include/mlir/Dialect/QCO/IR/QCOOps.h`) so every two-qubit operation (and,
where the target declares it, every group of qubits used by a native multi-qubit
operation — see the next paragraph) ends up on mutually adjacent target sites. A
`CompilerTarget` (declared in `mlir/include/mlir/Compiler/Target.h`) numbers its
hardware locations with two different kinds of identifier: a `SiteId`
(`int64_t`) is the identifier the target's author chose (for example, the number
written in a coupling list like `{2, 4}`), while a dense "vertex" index
(`size_t`, `0` to `numQubits() - 1`) is what routing algorithms use internally;
`vertexForSite(SiteId)` converts one to the other, and
`areAdjacent(size_t, size_t)`/`forEachNeighbour(size_t, callback)` operate on
the dense vertex form. `CompilerTarget::Coupling` is
`std::pair<SiteId, SiteId>`; the target's coupling list is returned by
`couplings()`. `mlir::qco::Layout` (declared in
`mlir/include/mlir/Dialect/QCO/Utils/Layout.h`) is the bijection between a
"program index" (a logical qubit's permanent allocation order in the source
program) and a "hardware index" (a dense vertex, in the sense above);
`layout.swap(hwA, hwB)` exchanges which program qubit occupies which of two
hardware vertices, which is what a routed SWAP physically does.

Routing divides the circuit into layers of independently executable operations
and runs an A* search to find a short SWAP sequence that makes the next layer
(plus `nlookahead` more layers) executable. Most operations in a layer are
ordinary two-qubit operations, each needing just one pair of program qubits to
land on mutually adjacent hardware sites, but the pass also natively routes a
wider operation (for example, a three-qubit `ccz` or `ccx`) without decomposing
it first, whenever the target's `CompilerTarget::operations` capability list
explicitly declares that exact operation by name and qubit count (this is
unrelated to the present change, but is current, load-bearing behavior, not a
two-qubit-only simplification): in that case, the operation's qubits are treated
as one group (called a "front" in the code, represented by `IndexGroupType`, a
`SmallVector<size_t, 3>` of program-qubit indices), and the search's goal check
(`Node::isGoal`) and heuristic (`Node::h`) require every pair of qubits within
that group to be mutually adjacent — a "clique"/triangle check for a three-qubit
group — instead of checking a single pair. A `Window`
(`SmallVector<IndexGroupType>`) is the sequence of upcoming layers/fronts the
search is currently looking at, sized `1 + nlookahead`. Regardless of how many
qubits a native operation spans, every SWAP the search inserts to satisfy it is
still an ordinary two-qubit exchange between two hardware vertices — this
ExecPlan's new edge-cost heuristic only ever prices individual SWAPs, so it is
unaffected by whether the layer being routed came from a two-qubit or a wider
native operation. The search itself is implemented by the private nested `Node`
struct and the `search()` member function inside the anonymous-namespace
`MappingPass` class in `Mapping.cpp`. Each `Node` wraps a candidate `Layout`
reached by a sequence of hypothetical SWAPs, using `IndexPairType`
(`using IndexPairType = std::pair<size_t, size_t>;`, declared near the top of
the anonymous namespace) to represent one SWAP as a pair of dense hardware
vertices, always stored with the smaller vertex first (`search()`'s
neighbour-expansion loop builds each candidate as
`const IndexPairType swap = std::minmax(hw0, hw1);`, so
`swap.first <= swap.second` always holds for every `Node::swap` value).
`Node::f = Node::g(alpha) + Node::h(window, target, params)` is the A* cost.
Today, `Node` already has a `float pathCost` field and a `bool useTypedCost`
field: when `useTypedCost` is true (meaning the `qubit-type-labels` option was
non-empty), the non-root constructor adds
`typedSwapCost(qubitLabels[prog0], qubitLabels[prog1])` to `pathCost` for the
program qubits the SWAP exchanges (looked up via
`layout.getProgramIndices(swap.first, swap.second)` before the SWAP is applied
to the layout), and `Node::g` returns
`alpha * (useTypedCost ? pathCost : depth)`.
`typedSwapCost(QubitLabel, QubitLabel)` is a private static helper on `Node`
returning `A<>A=1, A<>B=2, B<>B=3`; `QubitLabel` is an
`enum class QubitLabel : uint8_t { Auxiliary = 0, Data = 1 };` declared inside
`MappingPass`. `qubitLabels` is a `SmallVector<QubitLabel>` member of
`MappingPass`, parsed once per pass run inside `runOnOperation` by the existing
static helper `parseQubitLabels(StringRef spec, size_t nqubits)`, which returns
`FailureOr<SmallVector<QubitLabel>>`, failing the pass with a diagnostic (via
`func.emitError()` followed by `signalPassFailure()`) if `spec` contains a
character other than `A`/`a`/`0`/`B`/`b`/`1`. `search()` constructs the root
`Node` as `std::construct_at(arena.Allocate(), layout, !qubitLabels.empty());`
and each child `Node` (inside the neighbour-expansion loop) as:

    frontier.emplace(std::construct_at(arena.Allocate(), curr, swap, window,
                                        *target, params,
                                        ArrayRef(qubitLabels)));

passing the parent node, the candidate `swap`, the current lookahead `window`,
the `target`, a `Parameters params` value
(`struct Parameters { float alpha; float lambda; };`, constructed just above as
`const Parameters params{.alpha = alpha, .lambda = lambda};`), and
`ArrayRef(qubitLabels)`.

`MappingPass`'s pass options are declared once, in TableGen, in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` (the `MappingPass` def's
`options` list: `nlookahead`, `alpha`, `lambda`, `niterations`, `ntrials`,
`seed`, `qubitTypeLabels`). Building the project code-generates
`MappingPassOptions` (a plain struct with one field per option, constructible
with designated initializers such as `MappingPassOptions{.alpha = 2.0F}`) and
`MappingPassBase` (which owns one `mlir::Pass::Option<T>` per option field,
readable inside the pass as `this->cppName`, for example `this->alpha`) into
`build/<preset>/mlir/include/mlir/Dialect/QCO/Transforms/Passes.h.inc`, which
`Mapping.cpp` includes via `#include "mlir/Dialect/QCO/Transforms/Passes.h.inc"`
inside a `GEN_PASS_DEF_MAPPINGPASS` guard. Each TableGen
`Option<cppName, arg, type, default, description>` becomes a
`MappingPassOptions` field named `cppName` (of the given C++ type) and a
`MappingPassBase` member also named `cppName`.

Unit tests for this pass live in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`. Tests build an
MLIR module with `QCOProgramBuilder`
(`mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`), run the pass via
the `MappingPassFixture::runPass(ModuleOp, CompilerTarget, MappingPassOptions)`
static helper defined at the top of the test file, then assert on the resulting
IR, typically via a local `isExecutable(...)` helper (checks every two-qubit
operation sits on adjacent target sites) and/or by walking `SWAPOp`s directly
with `m->walk([&](SWAPOp) { ... })`. The existing
`StatefulSwapLabelsChangeRoutingChoice`,
`StatefulSwapLabelsPreferCheaperTypedSwap`, and
`InvalidQubitTypeLabelsFailsThePass` tests in that file are the direct precedent
for the new tests this plan adds: they prove the `qubit-type-labels` heuristic
changes routing choices and rejects malformed input, using a small,
purpose-built topology and program plus a helper (`traceProgramIdentities`) that
follows a program qubit's identity through inserted SWAPs.

## Plan of Work

1. `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`: add two new entries to
   `MappingPass`'s `options` list, after the existing `qubitTypeLabels` entry:
   `Option<"nnnEdges", "nnn-edges", "std::string", "\"\"", "...">` and
   `Option<"nnnCostMultiplier", "nnn-cost-multiplier", "float", "3.0F", "...">`.
   Write each option's description string to explain, in the same level of
   detail as the existing `qubitTypeLabels` description: that `nnn-edges` is a
   comma-separated list of `siteA-siteB` pairs (using the target's own site
   identifiers, exactly as passed to the `CompilerTarget` constructor's coupling
   list) naming coupling edges that should be treated as more expensive; that
   each named pair must already be a coupling edge of the target (that is,
   `target.areAdjacent` must hold for it once resolved to dense vertices), or
   the pass fails with a diagnostic; that an empty string (the default) disables
   the heuristic; and that `nnn-cost-multiplier` (default `3.0`, must be `> 0`)
   is the multiplicative cost penalty applied to a SWAP whose edge is named in
   `nnn-edges`, on top of whatever cost `qubit-type-labels` would otherwise
   assign it (or on top of the flat cost of `1` if `qubit-type-labels` is
   unset), and is only observable when `nnn-edges` is non-empty. Extend the
   `MappingPass` TableGen `description` block with a paragraph documenting the
   combined cost formula:

       cost(edge, labels) = (qubit-type-labels set ? typedSwapCost(labels) : 1)
                            * (edge listed in nnn-edges ? nnn-cost-multiplier : 1)

   matching the level of detail already given for the `qubit-type-labels`
   paragraph.

2. `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`:
   - Add a new static helper,
     `parseNnnEdges(StringRef spec, const CompilerTarget& target)`, returning
     `FailureOr<DenseSet<IndexPairType>>`, placed next to the existing
     `parseQubitLabels`. An empty `spec` returns an empty set (heuristic
     disabled). Otherwise, split `spec` on `,`; for each non-empty token, split
     on `-` into exactly two substrings, parse each as a
     `CompilerTarget::SiteId` (`int64_t`), and call `target.vertexForSite(id)`
     on each; if parsing fails, a token does not split into exactly two parts,
     or either `vertexForSite` call returns `std::nullopt`, return `failure()`.
     Once both site identifiers resolve to dense vertices `v0`/`v1`, require
     `target.areAdjacent(v0, v1)` to be true (catching a typo'd or non-existent
     edge) and return `failure()` if not. Insert `std::minmax(v0, v1)` into the
     result set (matching the `swap.first <= swap.second` canonical form
     `search()` already uses for every `IndexPairType`, so no further
     canonicalization is needed at lookup time). `DenseSet<IndexPairType>`
     (`IndexPairType` is `std::pair<size_t, size_t>`) needs LLVM's
     `DenseMapInfo` to be defined for `std::pair` of two hashable types; confirm
     this resolves without a custom specialization when writing the code (LLVM's
     `DenseMapInfo.h` ships a generic `std::pair` specialization that composes
     the specializations of its two members, and `size_t` already has one, so
     this is expected to work unmodified, but must be confirmed by actually
     compiling it).
   - Add a `DenseSet<IndexPairType> nnnEdges;` private member on `MappingPass`,
     next to the existing `SmallVector<QubitLabel> qubitLabels;` member.
   - In `runOnOperation`, immediately after the existing `qubitTypeLabels`
     parsing block (the one that calls `parseQubitLabels` and handles failure),
     add an analogous block that calls
     `parseNnnEdges(nnnEdges.getValue(), *target)` (note: `target` is the
     `std::optional<CompilerTarget>` member, already known to be set by this
     point since the existing `if (!target) { ... }` check runs earlier), and on
     failure emits a diagnostic via `func.emitError()` naming the offending
     `nnn-edges` value, calls `signalPassFailure()`, and returns — mirroring the
     existing `qubitTypeLabels` failure handling exactly. On success, move the
     parsed set into the `nnnEdges` member. Also assert `nnnCostMultiplier > 0`
     alongside the existing `assert(alpha > 0 && ...)` style asserts near the
     top of `runOnOperation`.
   - Extend `Node` with a new `bool useEdgeCost` field, alongside the existing
     `bool useTypedCost` field. Update the root constructor's signature to
     `Node(Layout layout, bool useTypedCost, bool useEdgeCost)`, initializing
     both fields directly (mirroring how `useTypedCost` is initialized today).
     Update the non-root constructor's signature to add two trailing parameters,
     `const DenseSet<IndexPairType>& nnnEdges, float nnnCostMultiplier`, and
     initialize `useEdgeCost(parent->useEdgeCost)` alongside the existing
     `useTypedCost(parent->useTypedCost)`. Change the cost-accumulation block
     from `if (useTypedCost) { pathCost += typedSwapCost(...); }` to: compute
     `base` as `typedSwapCost(...)` when `useTypedCost` is true and `1.0F`
     otherwise; compute `edgeMultiplier` as `nnnCostMultiplier` when
     `useEdgeCost` is true and `nnnEdges.contains(swap)` (using the
     already-canonical `swap` value directly, per the `std::minmax` note above)
     and `1.0F` otherwise; and, only when `useTypedCost || useEdgeCost` is true,
     add `base * edgeMultiplier` to `pathCost` (skipping the addition entirely
     when neither heuristic is active, so `pathCost` stays exactly `0` and
     unused, matching today's behavior bit-for-bit when both new/old heuristics
     are off). Update the private `g(float alpha) const` method's condition from
     `useTypedCost ? pathCost : depth` to
     `(useTypedCost || useEdgeCost) ? pathCost : depth`. Leave `typedSwapCost`
     and `h(...)` unchanged (see Decision Log).
   - Update `search()`'s root-node construction to:

         std::construct_at(arena.Allocate(), layout, !qubitLabels.empty(),
                            !nnnEdges.empty());

     and its child-node construction to:

         std::construct_at(arena.Allocate(), curr, swap, window, *target,
                            params, ArrayRef(qubitLabels), nnnEdges,
                            nnnCostMultiplier);

     threading the new `MappingPass` members through exactly like `qubitLabels`
     is threaded today. No other function needs to change: `RoutingBundle`,
     `insertSWAPs`, `generateLayout`, and `dispatch` are untouched, because the
     new state is looked up by hardware edge and program identity from
     pass-level members, the same way `qubitLabels` already is.

3. `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`: add three
   new `MappingPassFixture`-based tests, placed near the existing
   `StatefulSwap*` tests. `NnnEdgeCostChangesRoutingChoice` builds a small
   topology with two alternative equal-length (in raw hop count) routes between
   a pair of program qubits that need a two-qubit gate, where one route's only
   edge is named in `nnn-edges` and the other route's edges are not; it runs the
   pass twice, once with `nnn-edges` unset and once naming the expensive edge
   with a multiplier greater than 1, and asserts (via the existing
   `isExecutable` helper for validity, and by walking `SWAPOp`s' operand sites,
   the same way `StatefulSwapLabelsChangeRoutingChoice` walks them today) that
   the two runs choose different routes, with the edge-cost-aware run avoiding
   the named edge. `NnnEdgeCostComposesWithTyped Cost` sets both
   `qubit-type-labels` and `nnn-edges` together and asserts the routing choice
   matches the multiplicative formula rather than either heuristic alone (for
   instance, by constructing a scenario where an otherwise-cheap `A<>A` SWAP on
   the named edge becomes exactly as expensive as a `B<>B` SWAP on an unnamed
   edge, and showing the search treats them as a genuine tie or a specific
   predicted preference, the same way `StatefulSwapLabelsPreferCheaperTypedSwap`
   proves the existing multiplier table today).
   `InvalidNnnEdgesSpecFailsThePass` passes a malformed `nnn-edges` value (at
   minimum: unparsable syntax, and a syntactically valid pair that is not
   actually an edge of the target) and asserts `runPass` returns `failure()` for
   each case, mirroring `InvalidQubitTypeLabelsFailsThePass`. As with the
   precedent tests, the exact topology, program, and seed values are worked out
   empirically against the real build while implementing (hand-tracing the A*
   tie-break order is error-prone), and this section must be updated with the
   final concrete values once confirmed.

4. `CHANGELOG.md`: add an `### Added` entry describing the new `nnn-edges` /
   `nnn-cost-multiplier` options once a PR reference is available, following
   whatever placeholder convention the file uses for entries drafted before a PR
   number exists (check the current `CHANGELOG.md` header/entry conventions
   before writing it, the same way the `qubit-type-labels` change did).

## Concrete Steps

All commands run from the repository root.

1. Configure once, if not already configured:
   `./.agent/run.sh cmake --preset release`
2. Rebuild after each source edit, scoped to the mapping unit test binary to
   keep iteration fast:
   `./.agent/run.sh cmake --build --preset release --target mqt-core-mlir-unittest-mapping`
3. Run the focused suite while iterating, narrowing with a filter:

       ./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping \
         --gtest_filter='*NnnEdge*'

4. Once the new tests are green, run the full focused mapping suite (no filter)
   to confirm no regression:
   `./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping`
5. Run the broader test suite: `./.agent/run.sh ctest --preset release`
6. Lint: `./.agent/run.sh uvx nox -s lint`

## Validation and Acceptance

The new `NnnEdgeCostChangesRoutingChoice` test fails to compile on pre-change
code (the `nnn-edges` option does not exist yet) and, after the change, passes
by demonstrating that naming an edge in `nnn-edges` with a multiplier greater
than 1 measurably steers the A* search away from that edge for a program and
target where an equal-hop-count alternative exists. The new
`NnnEdgeCostComposesWithTypedCost` test demonstrates the multiplicative
combination with `qubit-type-labels` matches the documented formula rather than
either heuristic being silently ignored when both are set. The new
`InvalidNnnEdgesSpecFailsThePass` test demonstrates `runPass` returns
`failure()` for malformed input instead of silently misbehaving or crashing.
Every previously passing test in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` and in the rest
of the repository's test suite continues to pass unmodified with `nnn-edges`
left at its default empty string, demonstrating the option is purely additive
and opt-in, matching the explicit requirement that existing behavior is
preserved by default. `./.agent/run.sh uvx nox -s lint` passes.

## Idempotence and Recovery

Every step is a source edit plus a rebuild/rerun; none mutate persistent state
outside the working tree and the `build/` directory (itself git-ignored and safe
to delete and reconfigure from scratch with the commands in Concrete Steps if it
becomes inconsistent). No destructive git operations are part of this plan.

## Artifacts and Notes

Two brainstorm/options documents from the design discussion that produced this
ExecPlan are checked in at
`.agent/plans/ft-aware/plan_ft_aware_mapping_ pipeline.md` (a larger,
multi-phase plan for the surrounding FT-aware mapping pipeline, of which this
ExecPlan covers only the first phase) and
`.agent/plans/ft-aware/subplan_nn_nnn_cost_encoding.md` (the four implementation
options considered for this specific change). They are background material, not
required reading to execute this ExecPlan, which is self-contained.

For completeness and to preserve the alternative for later reconsideration (per
the first Decision Log entry), "Option B" from that discussion is recorded here
in full: instead of a pass-local edge set, extend `CompilerTarget::Coupling`
(currently `std::pair<SiteId, SiteId>`, declared in
`mlir/include/mlir/Compiler/Target.h`) into a small struct carrying a weight,
for example `{SiteId first; SiteId second; double weight = 1.0;}`, or add a
parallel weight-lookup accessor alongside the existing `couplings()` method.
`CompilerTarget`'s internal distance precomputation (`Storage::distances`,
consumed by `distanceBetween(size_t, size_t)`) currently runs an unweighted
breadth-first search once per target construction; supporting real edge weights
would mean switching that precomputation to a weighted shortest-path algorithm
(for example Dijkstra's algorithm) so that both `areAdjacent`-style adjacency
queries and a weighted `distanceBetween` remain available generically to every
pass built on `CompilerTarget`, not just `MappingPass`. This would also let
`Node::h` (the A* lookahead heuristic, deliberately left unweighted by this
plan, see Decision Log) become edge-weight-aware later, if that ever turns out
to matter. The tradeoff, and the reason it was not chosen now, is size of
change: Option B touches `Target.h` and `Target.cpp` (`Storage`, every one of
`CompilerTarget`'s four constructor overloads) and every existing piece of code
that builds a `CompilerTarget` coupling list as a plain `{a, b}` pair, including
`getRydbergIonTarget()` in `test_rydberg_ions.cpp` and any other target
constructed elsewhere in the repository — a far larger and more widely-reaching
diff than Option A's self-contained addition to `MappingPass` alone, for a fact
(which edges are physically more expensive) that, as of this writing, only
`MappingPass` needs to know about.

## Interfaces and Dependencies

New TableGen options in `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`,
added to `MappingPass`:
`Option<"nnnEdges", "nnn-edges", "std::string", "\"\"", ...>` and
`Option<"nnnCostMultiplier", "nnn-cost-multiplier", "float", "3.0F", ...>`,
generating `MappingPassOptions::nnnEdges` (default `""`),
`MappingPassOptions::nnnCostMultiplier` (default `3.0F`), and the corresponding
`MappingPassBase` members. New private members on the anonymous-namespace
`MappingPass` class in `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`:
`static FailureOr<DenseSet<IndexPairType>> parseNnnEdges(StringRef, const CompilerTarget&)`
and `DenseSet<IndexPairType> nnnEdges;`. Extended nested `Node` struct: new
field `bool useEdgeCost`; root constructor becomes
`Node(Layout, bool useTypedCost, bool useEdgeCost)`; non-root constructor gains
two trailing parameters,
`const DenseSet<IndexPairType>& nnnEdges, float nnnCostMultiplier`. No changes
to `mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h` (the
`createMappingPass(const CompilerTarget&, MappingPassOptions)` factory signature
is unchanged; both new options flow through the existing `MappingPassOptions`
struct). No changes to `mlir/include/mlir/Compiler/Target.h` or
`mlir/lib/Compiler/Target.cpp` (see Decision Log and Artifacts and Notes
regarding Option B).
