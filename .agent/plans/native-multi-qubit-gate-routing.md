## Teach MappingPass to place and dynamically route native multi-qubit gates

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

### Purpose / Big Picture

`MappingPass` (the `place-and-route` MLIR pass, `mlir::qco::createMappingPass`,
implemented in `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`) turns a
quantum program written with dynamically allocated ("program") qubits into one
that runs on specific hardware (a `CompilerTarget`,
`mlir/include/mlir/Compiler/Target.h` — essentially a graph of which physical
qubit "sites" are directly wired to which other sites; a "coupling" is one
such wire, i.e. one edge in that graph). Today, `MappingPass` refuses,
unconditionally, to route any gate acting on more than two qubits at once;
whoever calls it must first run a separate decomposition pass that rewrites
such a gate into a sequence of one- and two-qubit gates. That is a reasonable
default for most hardware, but it is wrong for hardware where a genuine,
non-decomposed three-qubit gate — for example a doubly-controlled-Z ("CCZ")
or a Toffoli ("CCX", doubly-controlled-X) — is directly executable whenever
its three qubits sit inside one shared, small, mutually-connected physical
zone. A real example of such hardware is a "Rydberg-ion" architecture (a
class of neutral-atom quantum computers), where three qubits held in one
local control zone can be addressed by one native three-qubit laser pulse
sequence.

After this change, a `CompilerTarget` can opt in to this behavior by
explicitly declaring, in its existing `operations` capability list (see
Context and Orientation for the exact mechanism), that it supports a specific
named operation at a specific qubit arity greater than two — for example,
`"ccx"` at 3 qubits. When it does, `MappingPass` accepts a matching
undecomposed gate (as built by `mlir::qco::QCOProgramBuilder::mcx`/`mcz`, see
Context and Orientation) without requiring decomposition, and routes it by
ensuring its qubits end up on a set of hardware sites that are all mutually
adjacent (a "triangle" for a three-qubit gate — three sites where every pair
is directly coupled) — inserting ordinary two-qubit SWAP operations first if
they do not already sit that way. Every target that does not explicitly
declare such an operation is completely unaffected: `MappingPass` keeps its
exact prior behavior (unconditional decomposition-required rejection) for it,
so this is a strictly additive, opt-in, backward-compatible change. The
observable proof lives in three new, small, hand-verified GoogleTest cases in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` (see Plan of
Work item 4): one shows a native three-qubit gate already sitting on a
triangle is accepted and left undecomposed; one shows the pass actively
inserts SWAPs to create a triangle when the gate's three qubits do not yet
sit on one; one shows a target that does not declare the gate as native still
requires decomposition first, exactly reproducing today's behavior — together
with the pre-existing `FailNestedHigherArityUnitary` test in the same file,
which needs no changes and continues to prove that every target without an
explicit `operations` declaration (the default, and every other test target
in that file) is entirely unaffected.

This ExecPlan covers only the router generalization itself, verified with
small, synthetic, hand-traceable programs and targets. A separate, checked-in
ExecPlan, `.agent/plans/rydberg-ion-bacon-shor-mapping-test.md`, builds on top
of the capability this plan delivers to compile a large, realistic circuit
(a 9-data/3-ancilla quantum-error-correction circuit) end-to-end onto a
12-qubit target with three native-gate zones; read that plan for that
end-to-end scenario, which assumes this plan is already complete.

**Explicit non-goal: gate-basis translation among equivalent native gates is
not part of this pass, and the policy that decides "is this gate routable"
is deliberately isolated behind one seam so that non-goal can be revisited
later without touching the router itself.** `MappingPass`, as extended by
this plan, only ever accepts a wider-than-two-qubit gate when a single,
dedicated, private predicate function (`isNativelyRoutable`, see Plan of
Work item 2) says so. Today that predicate implements "the gate is already,
exactly, one of the target's explicitly declared native operations, matched
by both name and arity" (see Plan of Work item 1 for precisely which shapes
`CompilerTarget::supports` recognizes) — so it does not attempt to recognize
that two different named operations of the same arity might be functionally
interchangeable (for example, that a CCX and a CCZ are related by
conjugating the target qubit with a Hadamard on each side, `CCX = H(target)
· CCZ · H(target)`), and it never rewrites one into the other. Concretely: if
a `CompilerTarget` declares only `"ccz"` as native (not `"ccx"`), and a
program contains a `builder.mcx`-built CCX gate, this pass rejects it with
the same "decompose it to one- and two-qubit operations first" diagnostic it
would give for any other unsupported wide gate — it does not rewrite the CCX
into an equivalent `H`/CCZ/`H` sequence itself. Any such gate-basis
translation between equivalent native multi-qubit gates must happen in a
pipeline stage that runs *before* `MappingPass`, as a prerequisite this plan
assumes but does not implement (see Surprises & Discoveries for whether that
prerequisite already exists elsewhere in the codebase today, and an open
scope question raised for the user in Decision Log).

Because `isNativelyRoutable` is the single, isolated place this policy is
decided (every caller inside `discoverComputation` goes through it, and
nothing elsewhere in the router re-implements or duplicates the check), a
future change could freely swap its definition from today's "exact
name+arity match" to a looser "arity- and connectivity-only" policy — for
example, "routable whenever the target declares *some* native operation at
this exact arity, regardless of which specific gate it names" — mirroring
how this router already treats *two*-qubit gates today (it never checks a
two-qubit gate's specific identity against the target's declared operations
at all; any two-qubit gate is routable as long as its two qubits can be made
adjacent, and a *separate*, already-existing, later pipeline stage,
`createTargetNativeSynthesis`, is responsible for translating it to the
target's actual native two-qubit gate afterward). Making that same
"arity/connectivity-only, identity-agnostic" policy the default for
wider-than-two-qubit gates as well is explicitly not what this plan
implements (see Decision Log for why), but the `isNativelyRoutable` seam
exists specifically so a future task can make exactly that change by editing
one function, without touching `Window`, `Node`, `getWindow`, `advance`, or
`search()` at all — none of the generalized routing machinery in this plan
cares *why* `isNativelyRoutable` returned `true`, only that it did.

### Progress

- [x] (2026-08-12T00:20Z) Read `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`
      in full (1716 lines) to find every place that assumes a routable gate
      acts on at most two qubits: the unconditional `unitary.getNumQubits() >
      2` rejection in `discoverComputation`; the `Window`/`IndexPairType` type
      used by the A* search (`Node::isGoal`, `Node::h`, the neighbor-expansion
      loop in `search()`); `getWindow`'s `indices[0]`/`indices[1]` reads;
      `advance()`'s pairwise `target->areAdjacent(hw0, hw1)` executability
      check; and `skipQubitPairBlock`'s fixed two-iterator signature.
- [x] (2026-08-12T00:25Z) Read `mlir/include/mlir/Dialect/QCO/Utils/Drivers.h`
      (`walkProgramGraph`) and confirmed its `ReadyMap` (`using ReadyMap =
      llvm::SmallDenseMap<Operation*, SmallVector<size_t>, 8>`) already tracks
      a generically-sized `SmallVector<size_t>` of wire indices per ready
      operation — exactly `UnitaryOpInterface::getNumQubits()` many, computed
      generically via a `TypeSwitch`. This "readiness"/layering machinery
      needs zero changes; only the pairwise-assuming consumers listed above
      do.
- [x] (2026-08-12T00:30Z) Read `mlir/include/mlir/Compiler/Target.h` and
      `mlir/lib/Compiler/Target.cpp` in full. Found the compatibility hazard
      recorded in the first Decision Log entry below:
      `CompilerTarget::Storage::supportsOperation` (`Target.cpp` line 447)
      contains `if (!operations) { return true; }`, matching the class
      documentation ("An absent operation set means that every operation is
      native"). Also found `CompilerTarget::supports(Operation*)` already
      special-cases a `qco.ctrl` operation with exactly one control, one
      target, and a single `qco.x`/`qco.z` body as `"cx"`/`"cz"`
      (`Target.cpp` lines 682-693) — the direct precedent this plan
      generalizes for the two-control (CCX/CCZ) case.
- [x] (2026-08-12T00:35Z) Confirmed `mlir::qco::CtrlOp`
      (`mlir/include/mlir/Dialect/QCO/IR/QCOOps.td`, starting at line 1108)
      exposes `getNumControls()`, `getNumTargets()`, `getNumBodyUnitaries()`,
      and `getBodyUnitary(size_t)`, and that `CtrlOp::getBaseSymbol()` returns
      the literal string `"ctrl"` (line 1173 of that file) — confirming the
      existing generic fallback path in `CompilerTarget::supports` can never
      recognize a two-control `qco.ctrl` as `"ccx"`/`"ccz"` without an
      explicit special case. Confirmed no `RCCZOp` exists anywhere in
      `QCOOps.td` (only the pre-existing, semantically different `RCCXOp`,
      a relative-phase Toffoli), so `builder.mcz` (producing a `qco.ctrl`
      with a `qco.z` body) is the only way to construct a genuine CCZ gate.
- [x] (2026-08-12T00:40Z) Confirmed `mlir::qco::Layout::getHardwareIndices`/
      `getProgramIndices` (`mlir/include/mlir/Dialect/QCO/Utils/Layout.h`)
      are already variadic templates, not fixed-arity pair accessors, so no
      change is needed there; generalized code below calls the existing
      single-argument `getHardwareIndex(size_t)` once per group element
      instead.
- [ ] Implement the `CompilerTarget::supports(Operation*)` extension in
      `mlir/lib/Compiler/Target.cpp` (Plan of Work item 1).
- [x] (2026-08-12T01:00Z) Per explicit user direction, traced the full
      existing compilation pipeline to check whether "a wider-than-two-qubit
      gate has already been reduced to the target's native gate set before
      it reaches the routing stage" already holds for the pre-this-task
      codebase, and to check what (if anything) exists today for
      translating between equivalent native gates of the same arity (the
      user's own example: CCX vs. CCZ). Findings recorded in Surprises &
      Discoveries below; summary: yes, trivially, for the *existing* code
      (today's native multi-qubit gate set is always empty, so "reduced to
      the native set" and "fully decomposed to one-/two-qubit gates" are the
      same thing), but this task's new capability will not be *reachable*
      through the standard/default compilation pipeline without a further
      change. Per explicit user confirmation, that further change (making
      `populateTargetCompilationPipeline`/`populateDecomposeMultiControlledPipeline`
      target-aware, and/or building a CCX-vs-CCZ gate-basis-translation
      pass) is explicitly out of scope for this plan — see Decision Log.
- [ ] Implement the `Mapping.cpp` generalization (Plan of Work item 2):
      the isolated `isNativelyRoutable` predicate; native-gate acceptance
      gate in `discoverComputation`; `IndexGroupType`/`Window`
      generalization; `Node::isGoal`/`Node::h` generalization;
      `getWindow`/`skipQubitPairBlock`/`advance()` generalization; `search()`
      neighbor-expansion generalization.
- [ ] Update `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`'s
      `MappingPass` `description` to document the new capability (Plan of
      Work item 3).
- [ ] Add the three new native-multi-qubit-gate `TEST_F` cases to
      `test_mapping.cpp` (Plan of Work item 4), with exact
      target/topology/program values pinned down empirically against the
      real build while implementing (see Validation and Acceptance).
- [ ] Build the focused mapping unittest binary and iterate until every new
      and pre-existing test in `test_mapping.cpp` passes (Concrete Steps).
- [ ] Run the full C++ suite (`ctest --preset release`) — this task touches
      shared, widely-used production code (`Target.cpp`, `Mapping.cpp`), so a
      full run, not just the focused mapping binary, is required.
- [ ] Run `./.agent/run.sh uvx nox -s lint`.
- [ ] Add a `CHANGELOG.md` `Unreleased`/`Added` entry.
- [ ] Final read-through of the diff against `AGENTS.md` and this plan; fill
      in `Outcomes & Retrospective`.

### Surprises & Discoveries

- Observation: `MappingPass::discoverComputation`'s `unitary.getNumQubits() >
  2` check (`Mapping.cpp` line 628) has no conditional branch on the target at
  all today — it always rejects, regardless of what any `CompilerTarget`
  declares. Evidence: the check sits inside `if (unitary.getNumQubits() > 2)
  { unitary.emitError() ...; return WalkResult::interrupt(); }` with no
  target lookup anywhere near it.
- Observation: naively gating "accept this native multi-qubit gate" purely on
  `target->supports(op)` would be a silent, wide-reaching regression.
  Because `Storage::supportsOperation` returns `true` unconditionally for any
  target built without an explicit `operations` argument — which describes
  essentially every existing test target in `test_mapping.cpp`, including
  `getSquareGridTarget`'s two-argument `CompilerTarget(numTarget,
  std::move(couplings))` call and the targets parameterizing the pre-existing
  `FailNestedHigherArityUnitary` test — extending `CompilerTarget::supports`
  to recognize the two-control `qco.ctrl` (CCX/CCZ) shape and then gating
  solely on `supports(op)` would make every one of those targets suddenly
  report "yes, native" for that shape, silently flipping
  `FailNestedHigherArityUnitary`'s expected failure into an accepted pass.
  See Decision Log for the fix.
- Observation: the existing, pre-this-task default compilation pipeline
  already guarantees that no wider-than-two-qubit gate ever reaches
  `MappingPass` at all, but not because it selects or translates to a native
  gate set — because it unconditionally eliminates every such gate before
  `MappingPass` runs, full stop. `populateTargetCompilationPipeline`
  (`mlir/lib/Compiler/TargetCompilation.cpp`, the pipeline used by the
  `mqt-cc --target=...` command-line flow — confirmed via
  `mlir/tools/mqt-cc/mqt-cc.cpp`, which calls it whenever a `--target` is
  given) runs, in order: `populateQCOCleanupPipeline`,
  `populateDecomposeMultiControlledPipeline(pm, 3)`, then
  `populateDefaultQCOOptimizationPipeline`, `qco::createFuseTwoQubitGates()`,
  `qco::createMappingPass(target, ...)`, `populateQCOCleanupPipeline` again,
  `qco::createTargetNativeSynthesis(target)`, `createCSEPass()`,
  `createRemoveDeadValuesPass()`, and finally
  `qco::createVerifyTargetConformance(target)`. Two things follow directly
  from this order: (1) `populateDecomposeMultiControlledPipeline(pm, 3)`
  (`mlir/lib/Support/Passes.cpp`) takes only a `minQubits` integer, no
  `CompilerTarget` at all, and unconditionally decomposes every operation
  with at least 3 qubits — it has no way to know or care that a target might
  declare `"ccx"`/`"ccz"` as native, so it decomposes them regardless; (2)
  this happens *before* `createMappingPass`, so today, prior to this plan,
  `MappingPass` genuinely never receives any gate wider than two qubits
  through this pipeline. This trivially satisfies "gates have already been
  reduced to the native multi-qubit gate set before reaching routing" for
  today's code, because today's native multi-qubit gate set (as far as
  `MappingPass` is concerned) is always empty — "decomposed to the native
  set" and "fully decomposed to one-/two-qubit gates" coincide.
- Observation: `createTargetNativeSynthesis` (`TargetSynthesisPass`,
  `mlir/lib/Dialect/QCO/Transforms/NativeSynthesis/TargetSynthesis.cpp`,
  which runs *after* `createMappingPass` in the pipeline above) only ever
  processes one- and two-qubit unitary operations — `planTargetSynthesis`
  (line 317) explicitly skips any operation where
  `unitary.getNumQubits() != 1 && unitary.getNumQubits() != 2`. So there is,
  today, no code anywhere in the repository that translates between
  equivalent same-arity native multi-qubit gates (the user's CCX-vs-CCZ
  example) — not before routing, not after. If this plan's `MappingPass`
  change lets a native CCZ or CCX gate survive routing intact, nothing
  downstream would attempt to canonicalize it to some other, differently
  named but equivalent native gate; it simply passes through
  `TargetNativeSynthesis` completely untouched (skipped by that arity
  check) and reaches the final `createVerifyTargetConformance` stage as-is.
- Observation, good news: `createVerifyTargetConformance`
  (`VerifyTargetConformancePass`, same file, the pipeline's *last* stage)
  already walks *every* `UnitaryOpInterface` operation regardless of arity
  (no `!= 1 && != 2` skip — confirmed by reading its walk callback directly)
  and rejects the module with `operation->emitError() << "target does not
  support operation ..."` unless `target.supports(operation)` is `true`.
  Because this is the exact same `CompilerTarget::supports(Operation*)`
  method this plan extends in Plan of Work item 1, this final safety-net
  check will, automatically and with zero further changes beyond this
  plan's own Target.cpp edit, correctly reject a stray CCX/CCZ gate that
  should not have survived to this point (e.g. one a target did not
  actually declare as native) — a second, independent confirmation of the
  same invariant `MappingPass`'s own `discoverComputation` check enforces
  earlier, at no extra implementation cost.
- Observation: because of the first observation above, this plan's new
  `MappingPass` capability will not be exercised by the standard
  `mqt-cc --target=...` command-line flow once this plan is complete, even
  though the pass itself will support it: `populateTargetCompilationPipeline`
  will keep calling `populateDecomposeMultiControlledPipeline(pm, 3)`
  unconditionally before `createMappingPass`, decomposing every CCX/CCZ gate
  away regardless of what the target declares, exactly as it does today. The
  capability will only be reachable via a hand-built pass pipeline that
  omits that decomposition step — which is exactly what this plan's own new
  `test_mapping.cpp` cases do, and what
  `.agent/plans/rydberg-ion-bacon-shor-mapping-test.md`'s test does too, but
  is not how a real end user compiling through `mqt-cc --target=...` would
  ever reach it. Whether to close that gap (by making
  `populateTargetCompilationPipeline`/`populateDecomposeMultiControlledPipeline`
  target-aware, and/or by building the CCX-vs-CCZ-style gate-basis-selection
  pass the user described as a "step prior to this") is recorded as an open
  question for the user in Decision Log; this plan does not resolve it
  unilaterally.

### Decision Log

- Decision: isolate the entire "may this wider-than-two-qubit gate be routed
  without decomposition" policy behind one private, dedicated predicate
  method on `MappingPass`, `bool isNativelyRoutable(Operation* op) const`
  (see Plan of Work item 2), called from exactly one place
  (`discoverComputation`'s width check) rather than inlining the
  `target->hasExplicitOperations() && target->supports(op)` condition
  directly at that call site. Rationale: explicit user direction, given
  after reviewing an earlier draft that inlined the condition, to make the
  pass's policy on gate *identity* (does it care whether a wide gate is
  specifically a CCX vs. a CCZ, or only that *some* native gate of that
  arity exists?) easy to change or remove later without touching the router
  itself. Every other piece of this plan's generalization (`Window`,
  `Node::isGoal`, `Node::h`, `getWindow`, `advance`, `search()`) only ever
  asks "is `isNativelyRoutable(op)` true", never re-derives the answer
  itself, so a future change from today's "exact name+arity match" policy to
  a looser "arity/connectivity-only, gate-identity-agnostic" policy (the
  same policy this router already applies to two-qubit gates, whose specific
  identity — CX vs. CZ vs. anything else — routing never inspects; see
  Purpose/Big Picture) is a change to the body of one function, not a
  re-plumbing of the router's generalized machinery. Date/Author:
  2026-08-12, implementing agent, per explicit user direction.
- Decision: do **not** update `populateTargetCompilationPipeline` or
  `populateDecomposeMultiControlledPipeline` to become target-aware, and do
  **not** build a gate-basis-selection pass (e.g. one that would translate
  CCX into `H`/CCZ/`H` when only CCZ is declared native) as part of this
  plan. Rationale: as recorded in Surprises & Discoveries, the narrow
  invariant the user asked to check ("3-qubit gates should already have been
  decomposed into the native gate set" before reaching this part of the
  pipeline) does already hold in the existing, pre-this-task code —
  trivially, because `populateDecomposeMultiControlledPipeline(pm, 3)` runs
  unconditionally before `createMappingPass` and today's native multi-qubit
  gate set is always empty, so every wide gate is always fully decomposed.
  What does *not* yet exist anywhere in the codebase is (a) any target-aware
  decision about *whether* to decompose a wide gate at all (today's
  decomposition is blind to what the target declares), or (b) any pass that
  translates between two *different* same-arity native gates (e.g. CCX to
  CCZ) — both are exactly the "step prior to this" the user described. Given
  the choice between leaving this out of scope, making the decomposition
  step target-aware, or additionally building a full CCX-vs-CCZ
  basis-translation pass, the user explicitly chose to leave both out of
  scope for this plan (recommended option, since the latter two are
  materially larger, separate pieces of work that would need their own
  ExecPlan). Consequently, `populateTargetCompilationPipeline`/
  `mqt-cc --target=...` continues to always decompose every 3+-qubit gate
  before `MappingPass` runs, exactly as before this plan, and this plan's
  new native-multi-qubit-gate routing capability is only reachable via a
  hand-built pipeline that omits that decomposition step — exactly what this
  plan's own new `test_mapping.cpp` cases do, and what
  `.agent/plans/rydberg-ion-bacon-shor-mapping-test.md`'s test does too.
  Should the standard pipeline need to exercise this capability in the
  future, that is a separate, not-yet-started task. Date/Author:
  2026-08-12, implementing agent, per explicit user confirmation.
- Decision: gate acceptance of a native multi-qubit (width > 2) unitary
  operation in `discoverComputation` on **both**
  `target->hasExplicitOperations()` **and** `target->supports(op)`, not
  `supports(op)` alone (this is `isNativelyRoutable`'s current
  implementation, per the decision above). Rationale: as recorded in
  Surprises & Discoveries,
  `supports(op)` alone returns `true` for any target built without an
  explicit `operations` list; requiring `hasExplicitOperations()` too means a
  target must affirmatively declare (opt into) its native operation set
  before width > 2 gates are ever accepted, so every target that omits
  `operations` keeps today's exact behavior with zero code-path changes for
  it. This mirrors the same "additive, opt-in-only" design already used for
  the unrelated `qubit-type-labels` stateful swap heuristic already shipped
  on this branch (see `.agent/plans/state-dependent-ab-swap-heuristic.md`): a
  new capability that does nothing unless a caller explicitly asks for it.
  Date/Author: 2026-08-12, implementing agent.
- Decision: recognize a `qco.ctrl` operation with exactly two controls, one
  target, and a single `qco.x`/`qco.z` body operation as the named operations
  `"ccx"`/`"ccz"` (each with `numQubits = 3`) inside
  `CompilerTarget::supports(mlir::Operation*)` in `mlir/lib/Compiler/Target.cpp`,
  directly generalizing the existing one-control case (`"cx"`/`"cz"`) a few
  lines above it in the same function, rather than inventing any other
  identification mechanism (e.g. a new attribute or a separate op). Rationale:
  this is the only way `builder.mcx`/`builder.mcz` (the only
  `QCOProgramBuilder` methods that build three-qubit Toffoli/CCZ gates — no
  dedicated `ccx`/`ccz` builder method exists) produce their gates, and
  `CtrlOp::getBaseSymbol()` returns the literal string `"ctrl"`, so the
  pre-existing generic fallback path can never recognize this shape on its
  own. Not generalizing further to three-or-more controls: this task only
  needs the two-control case, and inventing names for wider
  multi-controlled gates that nothing exercises or tests here would be
  speculative. Date/Author: 2026-08-12, implementing agent.
- Decision: generalize the A* search's `Window` type from
  `SmallVector<IndexPairType>` (`IndexPairType = std::pair<size_t, size_t>`)
  to `SmallVector<IndexGroupType>` where `IndexGroupType =
  SmallVector<size_t, 3>`, and generalize every pairwise check that consumes
  a window entry (`Node::isGoal`, `Node::h`, `advance()`'s executability
  check) to an all-pairs ("clique") check over the group, rather than adding
  a second, parallel three-qubit-specific code path alongside the existing
  two-qubit one. Rationale: as recorded in Surprises & Discoveries,
  `walkProgramGraph` already produces a `SmallVector<size_t>` of exactly the
  right length for any gate arity, so one generalized, arity-agnostic
  implementation is not meaningfully more code than a hardcoded "width is 2
  or 3" special case, and it avoids two parallel implementations of the same
  logic needing to stay in sync. Every generalized function is written so a
  two-element group produces bit-for-bit the same computation the original
  two-qubit-only code performed (single-pair `areAdjacent`/`distanceBetween`
  check, same decay-weighted sum in `h()`) — the concrete argument for why
  every existing (all two-qubit-only) `test_mapping.cpp` test continues to
  pass unmodified. `Node::swap` (always exactly one SWAP, i.e. always exactly
  two hardware sites) and the `IndexPairType`-based `restore`/`converge`
  control-flow helpers are intentionally untouched, since a SWAP is always a
  two-site operation regardless of the arity of the gate being routed
  toward. Date/Author: 2026-08-12, implementing agent.
- Decision: generalize `Node::h()`'s per-window-entry cost from a single
  pairwise `distanceBetween(hw0, hw1) - 1` term to the sum, over every pair
  within the group, of `distanceBetween(hw_i, hw_j) - 1`. Rationale: this is
  the most direct arity-agnostic generalization of the existing heuristic's
  documented intent ("the number of SWAPs a naive router would insert"), it
  collapses to exactly the original one-term formula for a two-element group,
  and the existing heuristic already documents itself as a layer-decayed
  approximation rather than a strict, formally admissible A* lower bound, so
  a same-character (not necessarily perfectly tight) approximation for wider
  groups is consistent with the existing design, not a new correctness
  requirement. Date/Author: 2026-08-12, implementing agent.
- Decision: rename `skipQubitPairBlock<Direction>(WireIterator&, WireIterator&)`
  to `skipQubitGroupBlock<Direction>(MutableArrayRef<WireIterator>)`,
  operating on an arbitrary-length group of wire iterators via an internal
  `SmallVector<WireIterator, 3>` working copy, but preserving its exact
  original control flow (advance each iterator independently past
  single-qubit gates to the next multi-qubit operation; if every iterator in
  the group lands on the *same* operation, commit that advance and repeat;
  otherwise stop without committing the last, divergent advance). The
  barrier special case (`isa<BarrierOp>(u) && u.getNumQubits() != 2`)
  generalizes to `u.getNumQubits() != block.size()`, using the group's
  actual size rather than a hardcoded `2`. Rationale: same
  one-generalized-implementation reasoning as the `Window` decision above.
  Date/Author: 2026-08-12, implementing agent.
- Decision: leave `search()`'s node-expansion iteration-budget formula
  (`b = maxDegree * ((numQubits + 1) / 2)`, `budget = min(b*b*b, cap)`)
  unmodified even though it was originally sized with two-qubit windows in
  mind. Rationale: this is a soft performance cap (with an explicit hard
  `cap = 25'000'000` ceiling already in place regardless), not a correctness
  requirement — retuning it is an optional performance consideration outside
  this task's scope. Date/Author: 2026-08-12, implementing agent.
- Decision: leave `computation.hasTwoQubitOperations` (a `Computation` field
  set in `discoverComputation` but, per a repository-wide grep, never read
  anywhere in the codebase — an apparently already-dead field predating this
  task) untouched rather than also making it consider width-3+ operations.
  Rationale: `AGENTS.md` directs agents to keep changes focused and avoid
  unrelated cleanup; since nothing reads this field today, touching it has
  no observable effect on this task. Date/Author: 2026-08-12, implementing
  agent.
- Decision: split this router-generalization work into its own ExecPlan
  file, separate from `.agent/plans/rydberg-ion-bacon-shor-mapping-test.md`
  (which uses the capability this plan delivers to compile a large, realistic
  circuit). Rationale: explicit user request, made after an initial combined
  draft, to keep the two concerns — "teach the router to handle native
  multi-qubit gates in general, proven with small synthetic tests" versus
  "use that capability to compile one specific, large, realistic circuit
  end-to-end" — as independently readable, independently landable units of
  work, each satisfying `.agent/PLANS.md`'s "one ExecPlan per independently
  implemented task" guidance. Date/Author: 2026-08-12, implementing agent.

### Outcomes & Retrospective

Not yet started; implementation begins after this plan is reviewed. This
section will be filled in once every new test passes and full validation
(Concrete Steps) is complete.

### Context and Orientation

**The pass and its target.** `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`
implements `MappingPass`, registered as pass `place-and-route`, which
"routes" a quantum program: it takes a program using dynamically allocated
("program") qubits and a `CompilerTarget`
(`mlir/include/mlir/Compiler/Target.h`) describing hardware qubit "sites" and
a "coupling" graph of which site-pairs are physically wired together, and
inserts `qco.swap` operations (`SWAPOp`,
`mlir/include/mlir/Dialect/QCO/IR/QCOOps.h`) so every gate in the program ends
up acting on qubits sitting on adjacent (for two-qubit gates) or, after this
change, mutually-adjacent (for wider native gates) sites. "Program index" (or
`prog`) names a logical qubit by its permanent allocation order; "hardware
index" (or `hw`) names a physical site's position (a small dense integer, 0
through `target.numQubits() - 1`) in the target's coupling graph;
`mlir::qco::Layout` (`mlir/include/mlir/Dialect/QCO/Utils/Layout.h`) is the
current bijection between the two, updated in place every time a SWAP is
chosen.

**Why CCZ/CCX need special handling.** Today, `discoverComputation` (a
private static — this task makes it a regular member so it can read
`this->target` — member function of the anonymous-namespace `MappingPass`
struct, around line 620 of `Mapping.cpp`) walks every operation in the
program and, upon finding any `UnitaryOpInterface` operation (the common
interface every quantum gate implements) whose `getNumQubits()` exceeds 2,
unconditionally fails with the diagnostic "cannot route an operation acting
on N qubits; decompose it to one- and two-qubit operations first" (verified
as the exact literal `unitary.getNumQubits() > 2` check, with no target
lookup nearby, in the current file). `mlir::qco::QCOProgramBuilder` has no
dedicated `ccx`/`ccz` method; a three-qubit CCZ or Toffoli/CCX gate is
instead built via the macro-generated `mc##OP_NAME(ValueRange controls, Value
target)` family in
`mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h` —
`builder.mcx({a, b}, c)` and `builder.mcz({a, b}, c)` — which each produce a
single `qco.ctrl` operation (`CtrlOp`, defined starting at line 1108 of
`mlir/include/mlir/Dialect/QCO/IR/QCOOps.td`) wrapping two controls, one
target, and a single-operation body (`qco.x` or `qco.z`);
`CtrlOp::getNumQubits()` returns `getNumControls() + getNumTargets()`, i.e. 3
for this shape. Because this is > 2, `discoverComputation` currently rejects
it outright, forcing a caller to run the separate `DecomposeMultiControlled`
pass (`decompose-multi-controlled`,
`mlir/lib/Dialect/QCO/Transforms/Decomposition/DecomposeMultiControlled.cpp`)
first, which rewrites the gate into one- and two-qubit gates. That is the
wrong behavior for a target where such a three-qubit gate is itself directly
executable in hardware — there, the gate should be routed (potentially
SWAPped into place) as one atomic three-qubit operation, never decomposed.

**How a target declares an operation "native", and the exact compatibility
hazard this task must avoid.** `CompilerTarget` has an optional `operations`
constructor argument (`std::optional<std::vector<Operation>>`, where
`Operation` bundles a name, a fixed qubit arity, and a parameter count — see
the class declaration around line 126 of `Target.h`). Its own documentation
states: "An absent operation set means that every operation is native; a
present empty set means that no hardware operation is native."
`CompilerTarget::hasExplicitOperations()` reports whether that optional was
ever given a value at all, and `bool CompilerTarget::supportsOperation(StringRef
name, size_t numQubits, std::optional<size_t> numParameters)` (implemented in
`mlir/lib/Compiler/Target.cpp`, delegating to `Storage::supportsOperation`)
implements exactly this contract: `if (!operations) { return true; }` — i.e.,
whenever a target was built without ever passing `operations` (the common,
convenient case: `getSquareGridTarget`'s `CompilerTarget(numTarget,
std::move(couplings))` and most other `test_mapping.cpp` target literals omit
it entirely), *every* named operation of *every* arity is reported as
supported, unconditionally. There is a second overload, `bool
CompilerTarget::supports(mlir::Operation* operation)`, which maps an actual
MLIR operation to a name/arity pair and calls `supportsOperation`
internally; it already special-cases a `qco.ctrl` with exactly one control,
one target, and a single `qco.x`/`qco.z` body as the named operations
`"cx"`/`"cz"` (each width 2) — see lines 682-693 of `Target.cpp`. If this
task's new native-multi-qubit-gate acceptance check in `discoverComputation`
consulted `target->supports(op)` alone, then *every* existing test target in
`test_mapping.cpp` would suddenly report "yes, a two-control `qco.ctrl`
(CCX/CCZ shape) is supported" as soon as this task also extends
`supports(Operation*)` to recognize that shape (which it must do, to
recognize CCZ/CCX at all) — silently breaking the pre-existing
`FailNestedHigherArityUnitary` test (which builds exactly a two-control
`qco.ctrl` via `builder.mcx` on such a target and asserts the pass still
fails with the decompose-first diagnostic). The fix (see Decision Log) is to
require `target->hasExplicitOperations()` to also be true before ever
consulting `supports(op)` for a width > 2 gate.

**The router's internal "windows" of pairwise-assumed gates, and how they
generalize.** Beyond `discoverComputation`'s width check, four more places
inside `Mapping.cpp` assume every routable multi-qubit gate has exactly two
operand qubits, and need generalizing to "however many the gate actually
has" (see Decision Log for the precise generalization chosen for each):

1. The `Window` type (`using IndexPairType = std::pair<size_t, size_t>; using
   Window = SmallVector<IndexPairType>;`, near the top of the anonymous
   `MappingPass` struct) represents a short lookahead sequence of "the next
   gate(s) in each qubit's future" as pairs of program indices. This task
   changes it to `using IndexGroupType = SmallVector<size_t, 3>; using Window
   = SmallVector<IndexGroupType>;` — a group instead of strictly a pair.
2. `Node::isGoal` currently checks one pairwise `areAdjacent`. It generalizes
   to checking every pair within the group is adjacent.
3. `Node::h` currently computes one pairwise `distanceBetween(hw0, hw1) - 1`
   term per window entry. It generalizes to summing that term over every
   pair within each window entry's group.
4. `getWindow` (builds the `Window` by walking `walkProgramGraph`'s
   already-generic `ReadyMap`) currently reads only
   `indices[0]`/`indices[1]`. It generalizes to mapping every element of
   `indices` through `infos.lookupProgram(...)` into the group.
5. `advance()` currently checks one pairwise `target->areAdjacent(hw0,
   hw1)`. It generalizes to checking every pair within the operation's
   mapped hardware sites.
6. `skipQubitPairBlock` (a helper `getWindow` uses to skip past runs of
   consecutive gates repeatedly involving the exact same qubit group)
   currently takes exactly two `WireIterator&` parameters. It generalizes to
   `skipQubitGroupBlock`, taking a `MutableArrayRef<WireIterator>`.
7. `search()`'s A* neighbor-expansion loop currently iterates the literal
   two-element list `{q0, q1}` destructured from `window.front()`. It
   generalizes to a plain range-based loop over `window.front()`.

Every one of these is written so that a two-element group reduces to exactly
the same computation the current, two-qubit-only code performs. `Node`'s own
`swap` field (`IndexPairType swap`) is untouched: a SWAP always exchanges
exactly two hardware sites, regardless of the arity of whatever gate the
search is trying to make executable, so `insertSWAPs`, `restore`, and
`converge` need no changes.

### Plan of Work

1. `mlir/lib/Compiler/Target.cpp`, inside `CompilerTarget::supports(::mlir::Operation*
   operation)`: extend the existing `if (auto controlled =
   dyn_cast<qco::CtrlOp>(operation); controlled && controlled.getNumControls()
   == 1 && controlled.getNumTargets() == 1 && controlled.getNumBodyUnitaries()
   == 1) { ... }` block (currently only handling the one-control case) to also
   handle exactly two controls, mapping a single `qco.x` body to `"ccx"`
   (width 3) and a single `qco.z` body to `"ccz"` (width 3), calling
   `storage_->supportsOperation("ccx", 3, 0)`/`storage_->supportsOperation("ccz",
   3, 0)` exactly as the existing one-control branch calls
   `storage_->supportsOperation("cx", 2, 0)`/`storage_->supportsOperation("cz",
   2, 0)`. Leave every other branch of `supports(Operation*)` untouched.

2. `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`:
   - Add a new private member function on the anonymous-namespace
     `MappingPass` struct, `bool isNativelyRoutable(Operation* op) const`,
     implemented as exactly `return target->hasExplicitOperations() &&
     target->supports(op);` and documented with a comment explaining it is
     the *only* place this pass decides whether a wider-than-two-qubit gate
     may be routed without decomposition, so that a future change to a
     looser, gate-identity-agnostic policy (see Purpose/Big Picture and
     Decision Log) only ever needs to edit this one function's body. This
     is a deliberate extraction, not an inline condition, per explicit user
     direction (see Decision Log).
   - Remove `static` from `discoverComputation`'s declaration (it becomes a
     regular member function so it can call `this->isNativelyRoutable`,
     which itself reads `this->target`, already guaranteed non-null by the
     `if (!target) { llvm::reportFatalUsageError(...); }` check earlier in
     `runOnOperation`) and change the `unitary.getNumQubits() > 2` branch to
     only fail when `!isNativelyRoutable(op)`; when that returns `true` (the
     target explicitly declared this exact operation shape as native), fall
     through without erroring, exactly as a `<= 2`-qubit operation already
     does today. Leave the diagnostic message's exact wording unchanged
     (verbatim "decompose it to one- and two-qubit operations first" — this
     substring is asserted on by the pre-existing `FailNestedHigherArityUnitary`
     test).
   - Add `using IndexGroupType = SmallVector<size_t, 3>;` alongside the
     existing `IndexPairType`, and redefine `Window` as
     `SmallVector<IndexGroupType>`. Keep `IndexPairType` itself (still used by
     `Node::swap`, `search()`'s return type, and `restore`/`converge`).
   - Change `Node::isGoal`'s parameter type from `const IndexPairType& front`
     to `const IndexGroupType& front`, and its body from the single
     `layout.getHardwareIndices(front.first, front.second)` +
     `target.areAdjacent(hw0, hw1)` check to a nested loop over every pair
     `(front[i], front[j])`, `i < j`, requiring `target.areAdjacent(...)` for
     all of them (returning `false` on the first failing pair).
   - Change `Node::h`'s per-window-entry term from a single
     `distanceBetween(hw0, hw1) - 1` to a sum, over every pair within that
     window entry's group, of `distanceBetween(hw_i, hw_j) - 1`, keeping the
     same per-window-entry `decay *= params.lambda` progression unchanged.
   - Change `getWindow`'s ready-operation branch: instead of reading
     `indices[0]`/`indices[1]` and constructing an `IndexPairType`, map every
     element of `indices` through `infos.lookupProgram(...)` into an
     `IndexGroupType`, push that onto `window`, and replace the
     `skipQubitPairBlock<Direction>(wires[i0], wires[i1])` call with a call to
     the new `skipQubitGroupBlock<Direction>` (see next bullet), passing
     copies of `wires[idx]` for every `idx` in `indices` and writing the
     results back.
   - Rename `skipQubitPairBlock` to `skipQubitGroupBlock` and change its
     signature from two `WireIterator&` parameters to one
     `MutableArrayRef<WireIterator>`; internally, replace the fixed
     `std::array block{it0, it1}` with a `SmallVector<WireIterator, 3>`
     built from the input range, and generalize the "all iterators reached
     the same next operation" check and the `isa<BarrierOp>(u) &&
     u.getNumQubits() != 2` special case (using `block.size()` instead of the
     literal `2`) to operate over the whole group instead of exactly two
     iterators; on success, copy the working `block` back into the caller's
     array.
   - Change `advance()`'s ready-operation branch: instead of reading
     `indices[0]`/`indices[1]` and checking one `target->areAdjacent(hw0,
     hw1)`, map every element of `indices` through
     `infos.lookupProgram(...)` then `layout.getHardwareIndex(...)` into a
     small vector of hardware indices, and release the operation only if
     every pair within that vector is `target->areAdjacent(...)`.
   - Change `search()`'s neighbor-expansion loop from
     `for (const auto& [q0, q1] = window.front(); const auto prog : {q0,
     q1})` to a plain `for (const auto prog : window.front())`, leaving the
     loop body unchanged.

3. `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`: extend `MappingPass`'s
   `description` field with a short paragraph documenting that a wider-than-
   two-qubit gate is now accepted without decomposition when the target's
   explicitly declared `operations` list includes it (by name and exact
   qubit arity), in which case the pass routes it by ensuring its qubits end
   up mutually adjacent (inserting SWAPs if necessary) rather than requiring
   decomposition first, matching the level of detail already given for the
   existing `qubit-type-labels` paragraph in the same description block.

4. `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`: add three
   new `TEST_F(MappingPassFixture, ...)` cases near the existing
   `qubit-type-labels` tests, each using a small, fully hand-traceable
   3-to-4-qubit target/program (exact values to be pinned down empirically
   against the real build while implementing — this repository's established
   practice for this file, per the `state-dependent-ab-swap-heuristic.md`
   plan's own notes on why hand-tracing A*/router behavior must be checked
   against a real build rather than derived purely on paper):
   - One test where a `CompilerTarget` explicitly declares
     `operations = {CompilerTarget::Operation("ccx", 3, 0)}` (or `"ccz"`) over
     a topology containing a triangle, a program consisting of exactly one
     `builder.mcx`/`builder.mcz` call whose three qubits are placed (via the
     pass's own initial layout, or a topology small enough that the only
     layout already satisfies it) on that triangle, asserting the pass
     succeeds, the module still verifies, and the routed IR still contains
     exactly one `CtrlOp` whose three sites are mutually adjacent per the
     target — proving the gate was accepted and left genuinely undecomposed.
   - One test using a topology where the initial layout does *not* already
     place the gate's three qubits on a triangle but at least one triangle is
     reachable via SWAPs, asserting the pass succeeds, at least one `SWAPOp`
     was inserted, and the final `CtrlOp`'s three sites are mutually
     adjacent — proving the pass actively routes (not merely accepts) native
     wider gates.
   - One test reusing the same program but with a `CompilerTarget` whose
     `operations` list is explicit yet does *not* include `"ccx"`/`"ccz"`
     (e.g. an empty list, or a list containing only unrelated
     two-qubit/one-qubit operations), asserting the pass still fails with a
     diagnostic containing "decompose it to one- and two-qubit operations
     first" — a direct regression check complementing the pre-existing
     `FailNestedHigherArityUnitary` test (which already covers the *default*,
     no-explicit-operations case and needs no changes).

5. `CHANGELOG.md`: add an `Unreleased`/`Added` (`✨`) bullet describing the new
   native-multi-qubit-gate routing capability, matching the format and
   placement convention of the existing `qubit-type-labels` entry already in
   this file.

### Concrete Steps

All commands run from the repository root.

1. If `build/release` is not already configured for this checkout, configure
   it once: `./.agent/run.sh cmake --preset release`. If it was configured
   from a different checkout path, delete and recreate it:
   `rm -rf build/release && ./.agent/run.sh cmake --preset release`.
2. After each source edit, rebuild the focused target:
   `./.agent/run.sh cmake --build --preset release --target mqt-core-mlir-unittest-mapping`.
3. While iterating on the `Target.cpp`/`Mapping.cpp` generalization and the
   three new tests, run just the new and directly related pre-existing
   tests:
   `./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping --gtest_filter='*NativeMultiQubit*:*FailNestedHigherArityUnitary*'`
   (exact new test names to be finalized as they are written; adjust the
   filter accordingly).
4. Once those pass, run the full focused mapping suite with no filter to
   confirm no regression anywhere in `test_mapping.cpp`:
   `./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping`
5. Because this task touches shared, widely-used production code
   (`Target.cpp`, `Mapping.cpp`), run the complete C++ test suite before
   declaring done, not just the focused mapping binary:
   `./.agent/run.sh ctest --preset release`
   Expected: 100% pass, with the same pre-existing configured skips recorded
   in the `state-dependent-ab-swap-heuristic.md` plan (2 QDMI skips) plus
   this task's new tests, and no new failures anywhere else in the tree.
6. Run `./.agent/run.sh uvx nox -s lint` and address any findings.

### Validation and Acceptance

- The three new `test_mapping.cpp` cases fail to compile before this change
  (no native-multi-qubit-gate acceptance path exists) and pass after it,
  demonstrating, at small hand-verified scale: (a) a target-declared native
  three-qubit gate already sitting on a triangle is accepted and left
  undecomposed; (b) such a gate not yet sitting on a triangle is actively
  routed there via inserted SWAPs; (c) a target that does not declare the
  gate as native still requires decomposition first, exactly as before.
- The pre-existing `FailNestedHigherArityUnitary` test continues to pass
  unmodified, demonstrating that every target without an explicit
  `operations` declaration is completely unaffected by this change — the
  concrete backward-compatibility proof for the `hasExplicitOperations()`
  gate described in the Decision Log.
- Every other pre-existing test in `test_mapping.cpp` continues to pass
  unmodified, demonstrating the `Window`/`Node`/`getWindow`/`advance`/
  `skipQubitGroupBlock` generalizations are behavior-preserving for every
  already-tested two-qubit-only scenario.
- The full C++ suite (`ctest --preset release`) passes with no new failures,
  demonstrating the `Target.cpp`/`Mapping.cpp` changes are safe across the
  rest of the codebase, not just this one test directory.
- `./.agent/run.sh uvx nox -s lint` passes on every new/changed file.

### Idempotence and Recovery

Every step is a source edit plus a rebuild/rerun; none mutate persistent
state outside the working tree and the git-ignored `build/` directory (safe
to delete and reconfigure from scratch via the commands in Concrete Steps if
it becomes inconsistent — no source or history is at risk). No destructive
git operations are part of this plan. Because this task edits shared,
widely-depended-on files (`Target.cpp`, `Mapping.cpp`, `Passes.td`), if an
edit partway through Plan of Work item 2 leaves the router in a
not-yet-self-consistent state (e.g. `Window`'s type changed but not every
consumer updated yet), the safe recovery is to keep editing forward through
the remaining bullets of that item before attempting a build, since the
bullets are interdependent by construction (a partial edit will not compile);
there is no risk to already-committed history either way.

### Artifacts and Notes

- Downstream consumer of this plan's capability, to be implemented after
  this plan is complete: `.agent/plans/rydberg-ion-bacon-shor-mapping-test.md`.
- Unrelated, already-shipped prior feature on this branch that this plan's
  changes must not disturb: `.agent/plans/state-dependent-ab-swap-heuristic.md`
  (the `qubit-type-labels` option; its `Node` fields `pathCost`/`useTypedCost`
  and `typedSwapCost` helper are untouched by this plan, since they key off
  `Node::swap`, not off window/group arity).

### Interfaces and Dependencies

- Modified: `bool CompilerTarget::supports(::mlir::Operation*)`
  (`mlir/lib/Compiler/Target.cpp`) — new two-control `qco.ctrl` case
  recognizing `"ccx"`/`"ccz"` at width 3, alongside the existing one-control
  `"cx"`/`"cz"` case at width 2. No change to any public header/signature.
- New: `mlir::qco::(anonymous namespace)::MappingPass::isNativelyRoutable`
  (private, the single isolated policy seam described in Purpose/Big Picture
  and Decision Log).
- Modified: `mlir::qco::(anonymous namespace)::MappingPass::discoverComputation`,
  `Node` (`isGoal`, `h`), `getWindow`, `skipQubitPairBlock` (renamed
  `skipQubitGroupBlock`, new signature), `advance`, `search` — all private to
  `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`'s anonymous namespace;
  no public header (`Mapping.h`) or `MappingPassOptions` field changes.
  `createMappingPass(const CompilerTarget&, MappingPassOptions)`'s signature
  is unchanged.
- Modified: `MappingPass`'s TableGen `description` in
  `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` (documentation only;
  no new `Option`).
- New tests: three `TEST_F(MappingPassFixture, ...)` cases in
  `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`.
- Modified: `CHANGELOG.md` (`Unreleased`/`Added` entry).
- Consumed, unmodified: `mlir::qco::QCOProgramBuilder::mcx`/`mcz`
  (`mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`);
  `mlir::qco::CtrlOp` accessors (`mlir/include/mlir/Dialect/QCO/IR/QCOOps.td`);
  `mlir::qco::Layout::getHardwareIndex`/`getHardwareIndices`
  (`mlir/include/mlir/Dialect/QCO/Utils/Layout.h`); `walkProgramGraph`/
  `ReadyMap` (`mlir/include/mlir/Dialect/QCO/Utils/Drivers.h`);
  `MappingPassOptions::qubitTypeLabels` (already-shipped, unmodified by this
  plan).

When you revise this ExecPlan, ensure the change is reflected across all
relevant sections above, and add a note here describing what changed and
why. This plan was split out of a combined first design (recorded in
`.agent/plans/rydberg-ion-bacon-shor-mapping-test.md`'s Decision Log) at the
user's explicit request, so the router-generalization work and the
Bacon-Shor end-to-end demonstration are independently readable and
independently landable.