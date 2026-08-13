# Compile the Bacon-Shor QEC circuit onto a 12-qubit Rydberg-ion target with native CCZ/CCX and the stateful A/B swap heuristic

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

This ExecPlan depends on, and must not begin until, the checked-in ExecPlan
`.agent/plans/native-multi-qubit-gate-routing.md` is complete. That plan teaches
`MappingPass` (the `place-and-route` MLIR pass) to accept and dynamically
SWAP-route a native multi-qubit gate (for example a doubly-controlled-Z, "CCZ",
or a Toffoli/doubly-controlled-X, "CCX") without requiring it to be decomposed
first, whenever the target `CompilerTarget` explicitly declares that operation
(by name and exact qubit count) in its `operations` capability list; every
target that does not declare such an operation keeps the pass's prior,
unconditional "decompose first" behavior. This plan does not repeat that plan's
internal design (which files inside `Mapping.cpp`/`Target.cpp` change, or why);
read `.agent/plans/native-multi-qubit-gate-routing.md` for that. This plan only
covers using that already-delivered capability to compile one specific,
realistic circuit.

## Purpose / Big Picture

A stand-alone Python research prototype at the repository root
(`find_acceptable_swaps.py`, `circuits.py`, `ftqc_circuits.py` — analysis
scripts, not part of the `mqt-core` Python package, not imported by any package
code) defines a 9-data-qubit "Bacon-Shor" quantum-error-correction (QEC) circuit
with 3 syndrome-extraction ancilla qubits (12 qubits total). The user separately
supplied, directly in this conversation, a 12-qubit "Rydberg-ion" hardware
connectivity graph (not present anywhere in the repository) whose coupling graph
contains exactly three triangles (three mutually-adjacent hardware sites) —
sized and shaped to match this exact circuit's 12 qubits and its six CCZ/CCX
correction gates.

After this change, a new C++ GoogleTest file,
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp`,
reconstructs that same Bacon-Shor circuit gate-for-gate using the MLIR QCO
dialect's `QCOProgramBuilder` C++ API, and compiles it for a `CompilerTarget`
describing the 12-qubit Rydberg-ion connectivity — with that target explicitly
declaring `"ccx"`/`"ccz"` at 3 qubits as native operations, per the capability
delivered by `.agent/plans/native-multi-qubit-gate-routing.md` — using
`MappingPass` with `qubit-type-labels` set so the 9 data qubits are labeled `B`
and the 3 ancilla qubits are labeled `A` (the already-shipped stateful A/B swap
heuristic from commit `388e58ab`, see
`.agent/plans/state-dependent-ab-swap-heuristic.md`). The observable outcome:
building and running the new test binary (`mqt-core-mlir-unittest-mapping`, the
same binary `test_mapping.cpp` already builds into) produces two new passing
GoogleTest cases proving the routed circuit is fully valid on the given hardware
graph — every two-qubit CNOT sits on two adjacent sites, and every three-qubit
CCZ/CCX sits on three mutually adjacent sites (a triangle) — while every CCZ/CCX
gate remains exactly the atomic three-qubit gate the circuit started with (no
decomposition ever happens), and that the same circuit still compiles when
`qubit-type-labels` is left at its default (unset).

### Progress

- [x] (2026-08-12T00:00Z) Read `AGENTS.md`,
      `.agent/plans/state-dependent-ab-swap-heuristic.md`,
      `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` in full,
      the existing stub at
      `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp`,
      `find_acceptable_swaps.py`, `circuits.py`, and `ftqc_circuits.py` (to
      extract the exact Bacon-Shor gate sequence), and the `QCOProgramBuilder`
      C++ header to confirm builder method names/signatures (`allocQubit`,
      `reset`, `h`, `cx`, `mcz`, `mcx`, `barrier`, `measure`, `sink`,
      `finalize`).
- [x] (2026-08-12T00:10Z) Wrote and had the user reject a first draft of this
      plan that assumed CCZ/CCX would be decomposed via the
      `decompose-multi-controlled` pass before routing. The user corrected this:
      on this Rydberg-ion architecture CCZ/CCX are native three-qubit gates and
      must not be decomposed; the pass only needs to ensure their three qubits
      end up mutually connected.
- [x] (2026-08-12T00:45Z) Split the router-generalization design (which parts of
      `Mapping.cpp`/`Target.cpp` change, and the `hasExplicitOperations()`
      backward-compatibility analysis) out into its own ExecPlan,
      `.agent/plans/native-multi-qubit-gate-routing.md`, at the user's explicit
      request, so this plan can stay focused purely on the Bacon-Shor
      circuit/test. See that plan for the router design.
- [x] (2026-08-13T01:30Z) Confirmed
      `.agent/plans/native-multi-qubit-gate-routing.md` is complete (its own
      Progress checklist fully checked, its own Concrete Steps run successfully
      — full CTest suite passing) before starting any work below.
- [x] (2026-08-13T01:35Z) Per explicit user correction, revised the plan (see
      Decision Log) to drop the erroneous final-measurement step and the barrier
      that depended on it: the Bacon-Shor circuit is a QEC *memory* round only
      and does not measure its own qubits.
- [x] (2026-08-13T02:00Z) Wrote
      `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp` per
      Plan of Work, and added it as a second source file to the existing
      `mqt-core-mlir-unittest-mapping` target in
      `mlir/unittests/Dialect/QCO/Transforms/Mapping/CMakeLists.txt`.
- [x] (2026-08-13T02:10Z) Built the focused mapping unittest binary and iterated
      until both new tests passed; hit and fixed three real,
      empirically-discovered issues along the way — a GoogleTest fixture-name
      collision across translation units; a user-flagged, suspiciously
      convenient `numSwaps == 0` result that turned out to be the routed circuit
      (including 90 real SWAPs `MappingPass` had genuinely inserted) being
      silently dead-code-eliminated by the shared `runPass` helper's cleanup
      step, because this measurement-free circuit had no observable effect; and
      a `CtrlOp`-counting bug that conflated the circuit's 36 one-control
      `CtrlOp`s (`builder.cx`/`cz`) with its 6 two-control CCX/CCZ gates — all
      three documented in Surprises & Discoveries and reflected in Decision Log.
- [x] (2026-08-13T02:15Z) Ran the full focused mapping suite (no filter): all 86
      tests pass (81 pre-existing, 3 added by the router plan, 2 added by this
      plan) — no regression in the pre-existing `test_mapping.cpp` cases.
- [x] (2026-08-13T02:20Z) Ran the full C++ suite
      (`./.agent/run.sh ctest --preset release`): 100% of 4595 tests passed,
      same 2 pre-existing configured QDMI skips, no regressions anywhere else in
      the tree.
- [x] (2026-08-13T02:25Z) Ran `./.agent/run.sh uvx nox -s lint`; failed only on
      the pre-existing, environment-specific `bibtex-tidy` hook (missing `node`,
      unrelated to this diff). Re-ran via
      `SKIP=bibtex-tidy ./.agent/run.sh prek run --all-files`; every other hook
      passed cleanly on the first attempt and was confirmed idempotent on a
      second run.
- [x] (2026-08-13T02:30Z) Final read-through of the diff against `AGENTS.md` and
      this plan; see Outcomes & Retrospective below.

All planned work is complete.

### Surprises & Discoveries

- Observation: `QCOProgramBuilder` has no `ccx`/`ccz` methods; three-qubit
  Toffoli/CCZ gates are instead built via the macro-generated
  `mc##OP_NAME(ValueRange controls, Value target)` family (e.g. `mcx`, `mcz`),
  declared once per base gate by the `DECLARE_ONE_TARGET_ZERO_PARAMETER` macro
  in `mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h` (visible via
  `grep -n "mc##OP_NAME" mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`).
  `mcz({a, b}, c)` realizes a CCZ gate; since CCZ's unitary
  (`diag(1,1,1,1,1,1,1,-1)`) is symmetric under permutation of all three qubits,
  which two of the three become "controls" and which becomes "target" in the
  builder call does not change the gate realized, so it is safe to always pass
  the two ancilla qubits as controls and the data qubit as target for the
  X-correction step even though the Python reference's `ccz(...)` argument order
  puts the data qubit first.
- Observation: `MappingPassOptions` is a C++20 aggregate whose fields must be
  initialized via designated initializers *in the struct's declaration order*
  (the order the corresponding `Option<...>` entries appear in `MappingPass`'s
  `options` list in `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`). That
  order is
  `nlookahead, alpha, lambda, niterations, ntrials, seed, qubitTypeLabels`, so a
  designated-initializer literal must write
  `.ntrials = ..., .qubitTypeLabels = ...` in that relative order, not the
  reverse, or the file fails to compile.
- Observation: the 12 qubits used across the four Bacon-Shor sub-circuits
  (`x_syndrome_circuit`, `correct_x_circuit`, `z_syndrome_circuit`,
  `correct_z_circuit` in `ftqc_circuits.py`) are exactly `state_qubits[0..8]` (9
  data qubits) and `qec_ancillas[0..2]` (3 ancilla qubits); the fourth qubit
  group, `ftswap_ancillas` (4 more qubits), is never referenced by any of those
  four methods and is therefore irrelevant to the circuit this task builds.
  Evidence: `grep -n "ftswap_ancillas" ftqc_circuits.py` matches only the
  `__init__` assignment, no other use in the file.
- Observation: listing every triangle (three sites where all three possible
  pairs are couplings) in the user-supplied 12-qubit Rydberg-ion graph (0<>1,
  1<>2, 2<>3, 2<>4, 3<>4, 4<>5, 5<>6, 5<>7, 6<>7, 7<>8, 8<>9, 8<>10, 9<>10,
  10<>11) finds exactly three: {2,3,4}, {5,6,7}, {8,9,10} — matching the
  circuit's 3 ancilla qubits and 6 CCZ/CCX gates (2 corrections per ancilla
  pairing) closely enough that this graph was evidently designed specifically as
  three native-three-qubit-gate zones connected by an otherwise plain linear
  chain (0-1-2, 4-5, 7-8, 10-11).
- Observation: naming this file's fixture class `MappingPassFixture` (an exact
  copy of `test_mapping.cpp`'s own fixture class name, both in anonymous
  namespaces) compiled cleanly but failed at *test-runtime*, not build time:
  `RUN_ALL_TESTS()` aborted every test in this file with "All tests in the same
  test suite must use the same test fixture class." GoogleTest's test suite
  identity is the class name as a *string*, checked globally across the whole
  process at registration time — anonymous-namespace C++ internal linkage (which
  does prevent an ODR violation) is irrelevant to it, since GoogleTest never
  compares C++ type identity, only the registered suite name. Evidence: the
  exact GoogleTest failure message, reproduced identically for both new tests
  before the fix. Fixed by renaming this file's fixture and
  parameterized-test-base classes to
  `RydbergIonMappingPassFixture`/`RydbergIonMappingPassTest` (only this file's
  copy; `test_mapping.cpp`'s own `MappingPassFixture`/`MappingPassTest` are
  untouched) — see Decision Log for why this supersedes the "duplicating
  same-named classes across TUs is safe" reasoning in an earlier Decision Log
  entry.
- Observation, corrected after user-flagged investigation (the first version of
  this entry, written before that investigation, wrongly attributed this to the
  SABRE layout refinement finding a genuine zero-swap solution — that conclusion
  was wrong and is superseded here): the *first* working version of
  `MapBaconShorCodeOnRydbergIonTarget` (built with `buildBaconShorCircuit`
  producing unmeasured, directly-sunk qubits, per the just-recorded
  no-measurement decision) reported `numSwaps == 0` and an empty routed module
  (`m->walk` found zero `CtrlOp`s and zero `SWAPOp`s; a full IR dump showed only
  `%c0_i64 = arith.constant 0; return %c0_i64`). The user found this suspicious
  and asked for the routed layout to be extracted and reviewed. Investigation
  (adding a temporary debug dump immediately after `pm.run()` for `MappingPass`
  alone, *before* the fixture's `runPass` helper's follow-up
  `applyPatternsGreedily(SinkOp::getCanonicalizationPatterns)` cleanup step)
  showed the routed module, at that point, still contained all 12 `qco.static`
  ops and **90** `qco.swap` ops — `MappingPass` itself had done real,
  substantial routing work. The entire circuit, including all 90 inserted SWAPs,
  was then silently deleted by the *follow-up* cleanup step: because this
  circuit's qubits were never measured and its `main` function returns only a
  hardcoded constant, the whole computation is legitimately unobservable dead
  code once QCO's `Pure`-marked gates are considered, and MLIR's greedy
  pattern-rewrite driver (invoked by `applyPatternsGreedily`, which performs its
  own dead-code elimination on the side, independent of which specific patterns
  are supplied) correctly erases it — every earlier assertion in the test
  (`isExecutable`, the `CtrlOp` arity checks) had been silently checking an
  empty function and passing vacuously. See Decision Log for the fix (measuring
  the 12 qubits as test-only instrumentation) and the next entry for the
  corrected, verified result.
- Observation: after adding measurement (see Decision Log), a first attempt at
  asserting `numNativeMultiQubitGates == 6` via
  `m->walk([&](CtrlOp ctrl) {...})` failed with `numCtrlOps == 42`, most
  reporting `getNumQubits() == 2`. Root cause: `builder.cx`/`builder.cz` are
  *also* built as a `CtrlOp` (with exactly one control), per
  `CompilerTarget::supports`'s existing one-control special case (see
  `.agent/plans/native-multi-qubit-gate-routing.md`'s Context and Orientation) —
  `qco.ctrl` is not exclusive to the two-control CCX/CCZ shape. 42 is exactly
  right: this circuit's 36 CNOTs (18 X-syndrome + 18 Z-syndrome) plus its 6
  CCZ/CCX gates. Fixed by filtering the walk to `ctrl.getNumControls() == 2`
  before counting/asserting arity.
- Observation: with both fixes above applied, `numSwaps` is genuinely **85**
  (not 0), and — per the user's request — the routed layout was extracted and
  reviewed directly: a temporary debug dump (tracking each live qubit's physical
  site forward through the routed IR, printing the three sites of every
  two-control `CtrlOp`) showed all 6 CCZ/CCX gates land on one of the target's
  declared triangles: five on `{5, 6, 7}` and one on `{8, 9, 10}` (never
  `{2, 3, 4}` — using every declared triangle is not a requirement, only landing
  on *some* valid triangle per gate is), and every triple is pairwise-adjacent
  per the target's coupling list, corroborating `isExecutable`'s independent,
  automated verification of the exact same property. 85 SWAPs for this ~90-gate
  circuit on this topology (a mostly linear chain with only two of its three
  triangles actually used by this particular routing) is plausible, not a red
  flag: each of the 3 ancillas interacts with 6 different, scattered data qubits
  per syndrome round, a much denser interaction pattern than the sparse target
  topology directly supports without relayouting. This debug instrumentation was
  removed after verification; it is not part of the committed test.

### Decision Log

- Decision: split the router-generalization work into its own ExecPlan,
  `.agent/plans/native-multi-qubit-gate-routing.md`, referenced here rather than
  duplicated, per explicit user request. Rationale: keeps "teach the router to
  handle native multi-qubit gates in general" (proven with small, synthetic,
  hand-traceable tests) and "use that capability to compile one large, realistic
  circuit end-to-end" as independently readable, independently landable units,
  matching `.agent/PLANS.md`'s "one ExecPlan per independently implemented task"
  guidance. Date/Author: 2026-08-12, implementing agent.
- Decision: build both the CCZ and CCX gates in this test circuit using
  `builder.mcz`/`builder.mcx` (the `qco.ctrl`-based shape the router plan
  teaches `MappingPass`/`CompilerTarget::supports` to recognize as
  `"ccz"`/`"ccx"`), not the pre-existing, already-atomic `RCCXOp` (`qco.rccx`, a
  "relative-phase" Toffoli) for the CCX gates. Rationale: `RCCXOp` implements a
  *different*, cheaper unitary (correct only up to an uncorrected phase on some
  computational basis states) — using it would silently change the quantum
  semantics of the ported Bacon-Shor circuit relative to the Python reference's
  exact `ccx(...)`/`ccz(...)` calls, which this task should not introduce
  silently. There is also no `RCCZOp` at all (confirmed by grep of `QCOOps.td`),
  so the CCZ gates need the `builder.mcz`/`qco.ctrl` path regardless; using the
  same path uniformly for both CCX and CCZ keeps the circuit construction
  symmetric and easy to verify against the Python reference gate-for-gate.
  Date/Author: 2026-08-12, implementing agent.
- Decision: give `getRydbergIonTarget()`'s `CompilerTarget` an explicit
  `operations` list containing exactly two entries,
  `CompilerTarget::Operation("ccx", 3, 0)` and
  `CompilerTarget::Operation("ccz", 3, 0)`, and nothing else (no `"cx"`, `"h"`,
  `"reset"`, etc.). Rationale: per
  `.agent/plans/native-multi-qubit-gate-routing.md`'s design, `Mapping.cpp` only
  ever consults `target->supports(...)` for gates wider than two qubits; every
  one- and two-qubit gate in this circuit (Hadamards, resets, the 36 CNOTs, and
  the SWAPs the pass itself inserts) routes exactly as it does for every other
  test target in this file, regardless of what the `operations` list does or
  does not mention. Declaring only the two operations this test actually needs
  is therefore both sufficient and the minimal, most legible expression of "this
  hardware's two native three-qubit gates are CCX and CCZ." Date/Author:
  2026-08-12, implementing agent.
- Decision: duplicate (rather than share via a new common header) the
  `isExecutable`/`getQubitValues` static helper functions from
  `test_mapping.cpp` into `test_rydberg_ions.cpp`, but *generalize* this file's
  copy of `isExecutable` to check all-pairs mutual adjacency for a
  wider-than-two-qubit gate (gathering every mapped hardware site via
  `unitaryOp.getInputQubits()` and requiring every pair among them to be
  adjacent) instead of `test_mapping.cpp`'s original, which asserts
  `unitaryOp.getNumQubits() <= 2` and checks exactly one pair. Rationale: both
  are anonymous/`static`-linkage helpers scoped to their own translation unit,
  so duplicating them across the two `.cpp` files linked into one binary is not
  an ODR violation; `test_mapping.cpp`'s own copy is left completely untouched
  (its own tests never exercise a wider-than-two gate, so its existing `<= 2`
  assertion remains valid and this task should not touch a file it does not need
  to touch), while this file's copy needs the generalized version to correctly
  validate this circuit's CCZ/CCX gates. Date/Author: 2026-08-12, implementing
  agent.
- Decision: label program qubits by allocation order — the 9 data qubits first
  (indices 0-8, labeled `B`), then the 3 ancilla qubits (indices 9-11, labeled
  `A`) — matching `BaconShorCodeCircuitGenerator.__init__`'s `state_qubits`
  (built first) then `qec_ancillas` (built second) order, and matching how
  `qubit-type-labels` indexes program qubits by permanent allocation order (see
  `.agent/plans/state-dependent-ab-swap-heuristic.md`'s Decision Log).
  Date/Author: 2026-08-12, implementing agent.
- Decision: do not attempt to prove, inside this test, that the state-dependent
  heuristic produces a measurably different SWAP sequence than the default
  flat-cost heuristic for this specific, large (~90-gate) circuit/target pair;
  that claim is already covered by `test_mapping.cpp`'s existing, small,
  hand-verified `StatefulSwapLabels*` tests. This test's job is the separate
  integration claim that the already-proven heuristic and the
  native-multi-qubit-gate routing capability can be used *together* to compile a
  real, motivating circuit end-to-end into a fully valid routed program.
  Date/Author: 2026-08-12, implementing agent.
- Decision: the circuit built by `buildBaconShorCircuit` does **not** measure
  its 12 qubits at the end; they are sunk directly (unmeasured) once the QEC
  round (X-syndrome, X-correction, Z-syndrome, Z-correction) completes, using
  the no-argument `builder.initialize()`/`builder.finalize()` overloads
  (matching the pattern already used by `test_mapping.cpp`'s
  `MapMixedScalarAndTensorAllocations` test). Confirmed by reading
  `QCOProgramBuilder.cpp`: `initialize()`'s no-argument overload still defaults
  to one `i64` return type, and `finalize()`'s no-argument overload
  correspondingly synthesizes a constant `0` return value for it — so the
  generated function does still return a (meaningless, unused) `i64`, just no
  classical measurement-derived value, which is the property this task actually
  needs. Rationale: explicit user correction — the Bacon-Shor circuit in
  `find_acceptable_swaps.py`/`ftqc_circuits.py` is a QEC *memory* round only; it
  never measures its own qubits. Measurement is a separate concern, used only in
  conjunction with some *other*, not-built-here state-preparation circuit, to
  check data integrity before/after running this memory circuit — it is not part
  of the Bacon-Shor circuit itself, and this task does not build a
  state-preparation/fidelity-check harness. An earlier draft of this plan
  incorrectly measured all 12 qubits at the end, treating this circuit like the
  unrelated pre-existing `MapFlatGHZ`/`MapGroverLike` tests (which *do* end
  their circuits with measurements, appropriately, since GHZ/Grover are
  themselves measurement-terminated algorithms) — that pattern does not apply
  here. The barrier this plan's first draft inserted "immediately before the
  final measurements" is dropped along with those measurements, since its stated
  rationale no longer applies; nothing else in this plan depended on it.
  Date/Author: 2026-08-13, implementing agent, per explicit user correction.
  **Superseded in part, later the same day:** `buildBaconShorCircuit` itself
  still never measures anything, and still matches this decision exactly, but a
  later Decision Log entry below (after a user-flagged investigation, see
  Surprises & Discoveries) adds measurement of the 12 qubits back — in a
  *separate*, test-only wrapper function, not inside `buildBaconShorCircuit` —
  because without it the routed circuit is entirely eliminated as dead code
  before any test assertion runs. Both decisions coexist: the circuit itself
  stays measurement-free; the test harness around it does not.
- Decision: rename this file's `MappingPassFixture`/`MappingPassTest` classes to
  `RydbergIonMappingPassFixture`/`RydbergIonMappingPassTest`, superseding the
  earlier Decision Log entry that argued duplicating `test_mapping.cpp`'s
  fixture class name across the two translation units linked into one binary was
  safe. Rationale: that argument was correct about C++ (anonymous namespace
  internal linkage prevents an ODR violation) but wrong about GoogleTest, which
  was empirically found to key test-suite identity on the class name as a plain
  string, checked globally across the whole process — see Surprises &
  Discoveries for the exact failure and evidence. Every other part of the
  "duplicate small helpers instead of sharing a header" decision (for
  `isExecutable`) is unaffected, since free functions in an anonymous namespace
  do not face this problem (only GoogleTest's own macro-generated class
  registration does). Date/Author: 2026-08-13, implementing agent.
- Decision: consolidate the shared "build the circuit, measure and sink all 12
  qubits, finalize" setup used by both tests into one helper,
  `buildAndFinalizeBaconShorProgram`, rather than duplicating it inline in each
  `TEST_F`. Rationale: this was not explicitly specified in the original Plan of
  Work item 4/5 wording (which described each test's setup separately), but the
  two tests' setup is otherwise identical apart from the `MappingPassOptions`
  passed to `runPass`, so factoring it out is a direct, small application of the
  "smallest change that fully solves the problem" principle in `AGENTS.md`
  rather than a scope expansion (the measurement this helper performs was added
  afterward, as test-only instrumentation — see the next Decision Log entry —
  but the "one shared helper" structure itself predates and is independent of
  that fix). Date/Author: 2026-08-13, implementing agent.
- Decision: measure all 12 qubits at the end of
  `buildAndFinalizeBaconShorProgram` (returning them as the module's classical
  result via `finalize(bits)`), as test-only instrumentation layered *on top of*
  `buildBaconShorCircuit`'s output, not by adding measurement inside
  `buildBaconShorCircuit` itself. Rationale: per the investigation recorded in
  Surprises & Discoveries, without some observable use of the routed circuit's
  qubits, the fixture's shared `runPass` helper's post-pass cleanup step
  legitimately treats the entire computation as dead code and erases it, making
  every assertion in these tests vacuous. Adding the measurement only in this
  test-only wrapper function (immediately before sinking, after calling the
  unchanged, semantically-faithful `buildBaconShorCircuit`) keeps the actual
  circuit-under-test (matching the Python reference gate-for-gate, still with
  zero measurements of its own) cleanly separated from this necessary piece of
  test scaffolding, and keeps the fixture's standard, already-proven `runPass`
  helper directly reusable (consistent with every other test in this file and in
  `test_mapping.cpp`) rather than needing a special-cased, cleanup-skipping run
  path just for this file. Date/Author: 2026-08-13, implementing agent,
  following up on the user-flagged investigation.
- Decision: keep `EXPECT_GT(numSwaps, 0U)` in
  `MapBaconShorCodeOnRydbergIonTarget` (an earlier, mistaken version of this
  entry — written before the investigation above — proposed dropping it; that
  proposal is superseded and was never actually applied to committed test code).
  Rationale: with the measurement fix in place, this assertion is no longer
  vacuous and is now backed by a directly observed, reviewed result: 85 real
  SWAPs, with every one of the 6 CCZ/CCX gates confirmed (both by the automated
  `isExecutable` check and by the manual layout extraction requested during
  review) to land on one of the target's declared triangles. Date/Author:
  2026-08-13, implementing agent.
- Decision: filter the `CtrlOp` walk in `MapBaconShorCodeOnRydbergIonTarget` to
  `ctrl.getNumControls() == 2` before counting/asserting arity, and rename the
  counter to `numNativeMultiQubitGates`, rather than walking every `CtrlOp` in
  the module. Rationale: as recorded in Surprises & Discoveries, `builder.cx`/
  `builder.cz` are themselves one-control `CtrlOp`s, so an unfiltered walk
  counts the circuit's 36 CNOTs alongside its 6 CCZ/CCX gates; the two-control
  filter isolates exactly the native multi-qubit gates this test's arity
  assertion is actually about. Date/Author: 2026-08-13, implementing agent.

### Outcomes & Retrospective

All Purpose/Big Picture goals were achieved, after a course correction and a
user-flagged investigation that materially improved the tests' rigor. The
9-data/3-ancilla Bacon-Shor QEC circuit, built gate-for-gate from the Python
reference in `find_acceptable_swaps.py`/`ftqc_circuits.py` (never measuring its
own qubits, exactly matching that reference — the earlier draft's erroneous
final measurement, added before the user's correction, is fully removed from
`buildBaconShorCircuit`), compiles through `MappingPass` for the 12-qubit
Rydberg-ion target with the state-dependent `qubit-type-labels` heuristic
engaged, with all 6 CCZ/CCX correction gates surviving routing as genuinely
native, undecomposed three-qubit `CtrlOp`s placed on mutually adjacent hardware
triples — verified two independent ways: automatically, by `isExecutable`'s
generalized all-pairs adjacency check, and manually, by extracting and reviewing
the actual routed layout at the user's request, which confirmed 85 real
`SWAPOp`s and all 6 native gates landing on one of the target's two most-used
triangles (`{5,6,7}` and `{8,9,10}`).

The user's suspicion about an initial `numSwaps == 0` result was entirely
correct and caught a real, otherwise-silent test defect: without measuring the
circuit's qubits, the entire routed computation — including all 90 SWAPs
`MappingPass` had genuinely inserted — was dead-code-eliminated by the shared
test fixture's own post-pass cleanup step before any assertion ran, making every
check in the test (including `isExecutable`) pass vacuously over an empty
function. The fix, arrived at without reintroducing the earlier, now-corrected
mistake of measuring inside the circuit itself, was to add measurement only in a
separate, clearly-documented test-instrumentation wrapper
(`buildAndFinalizeBaconShorProgram`) layered on top of the unchanged,
semantically-faithful `buildBaconShorCircuit`. A second, related defect
(counting all `CtrlOp`s instead of only the two-control CCX/CCZ shape,
conflating the circuit's 36 CNOTs — themselves built as one-control `CtrlOp`s —
with its 6 native multi-qubit gates) was caught by the same investigation and
fixed alongside it. Both defects are documented in detail in Surprises &
Discoveries, together with the concrete evidence (exact counts, exact failure
messages) that distinguishes them from the routing pass itself, which behaved
correctly throughout.

A separate, purely mechanical defect — reusing `test_mapping.cpp`'s own fixture
class name `MappingPassFixture` in this file's anonymous namespace — compiled
without any diagnostic but failed every test in this file at GoogleTest
registration time, since GoogleTest keys test-suite identity on the class name
as a string, not C++ type identity. This is recorded as a correction to this
plan's own earlier reasoning (the "duplicating same-named anonymous-namespace
classes across translation units is safe" argument was right about the C++
language and wrong about the test framework built on top of it).

Full validation was completed: the full focused mapping suite (86/86 passing: 81
pre-existing, 3 added by `.agent/plans/native-multi-qubit-gate-routing.md`, 2
added by this plan), the complete CTest suite (4595/4595 passing, the same 2
pre-existing configured QDMI skips), and the full `prek` hook set (every hook
passed on the first attempt for this plan's changes and was confirmed idempotent
on a rerun; only the environment's missing `node` binary — unrelated to this
diff — kept the `bibtex-tidy` hook from running).

The main lesson, echoing the same lesson already recorded in
`.agent/plans/native-multi-qubit-gate-routing.md`'s own retrospective: a "the
pass succeeded and every assertion passed" result is not, by itself, evidence
that a test exercised anything meaningful — an empty or dead-code-eliminated
program trivially satisfies almost any structural assertion. Treating a
suspiciously convenient result (zero SWAPs on a dense, ~90-gate circuit routed
onto a sparse topology) as worth investigating rather than accepting, and
independently extracting and reviewing the actual compiled artifact rather than
trusting the assertions alone, is what caught this — a concrete instance of the
broader practice this repository's prior ExecPlans already document valuing.

### Context and Orientation

**MappingPass and the capability this plan depends on.** See
`.agent/plans/native-multi-qubit-gate-routing.md`'s Context and Orientation for
the full internal design; the summary needed here is: a `CompilerTarget`
(`mlir/include/mlir/Compiler/Target.h`) can pass an explicit `operations` list
(a `std::vector<CompilerTarget::Operation>`, each bundling a name, a fixed qubit
arity, and a parameter count) to its constructor; if it explicitly lists an
operation named e.g. `"ccx"` with `numQubits = 3`, then `MappingPass`
(`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`) accepts a matching,
undecomposed three-qubit gate — built via `QCOProgramBuilder::mcx`/`mcz` (see
below) — and routes it by inserting ordinary two-qubit SWAPs, if necessary,
until its three qubits sit on three mutually-adjacent hardware sites (a
"triangle" in the target's coupling graph). A target that omits `operations` (or
that declares one that does not include `"ccx"`/`"ccz"` at width 3) keeps the
pass's prior, unconditional behavior: any gate wider than two qubits is rejected
with a "decompose it to one- and two-qubit operations first" diagnostic.

**Builder API.** Programs are constructed in C++ using `QCOProgramBuilder`
(`mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`). It follows "value
semantics": each gate-applying method consumes one or more current qubit SSA
("Static Single Assignment" — each variable is written exactly once) values and
returns new SSA values representing the qubits' state after the gate, so call
sites look like `q = builder.h(q);` or `std::tie(q0, q1) = builder.cx(q0, q1);`.
Relevant methods: `Value allocQubit()` (allocates one fresh qubit initialized to
`|0>`, implicitly assigning it the next unused "program index" — the identity
`qubit-type-labels` indexes by, in the order `allocQubit()` is called);
`Value reset(Value qubit)`; `Value h(Value qubit)`;
`std::pair<Value, Value> cx(Value control, Value target)` (controlled-X/CNOT);
`std::pair<ValueRange, Value> mcx(ValueRange controls, Value target)` and
`std::pair<ValueRange, Value> mcz(ValueRange controls, Value target)`
(multi-controlled X/Z; with exactly two controls these realize Toffoli/CCX and
doubly-controlled-Z/CCZ respectively — there is no separate three-qubit-native
`ccx`/`ccz` method, these `mc*` methods are the only way to build one);
`QCOProgramBuilder& sink(Value qubit)` (marks a qubit's SSA value consumed for
the last time, required by QCO's "linear typing" discipline before `finalize()`
— a qubit may be sunk directly, without first measuring it, as already
demonstrated by the pre-existing `MapMixedScalarAndTensorAllocations` test in
`test_mapping.cpp`); `void initialize()` (the no-argument overload; must be
called once first — it defaults to declaring one `i64` return type, per
`mlir/lib/Dialect/QCO/Builder/QCOProgramBuilder.cpp`); and
`OwningOpRef<ModuleOp> finalize()` (the no-argument overload, which synthesizes
a constant `0` for that `i64` return automatically — appropriate here since this
circuit's qubits are never measured and there is no other classical value to
return; see Decision Log).

**Test infrastructure.** Tests live in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/`, built as CMake target
`mqt-core-mlir-unittest-mapping` (`CMakeLists.txt` in that directory currently
lists only `test_mapping.cpp` as a source). An untracked stub,
`test_rydberg_ions.cpp`, already exists in that directory (visible in
`git status`) containing only the shared license header, includes,
`using namespace` declarations, the `getQubitValues` helper, and the
`MappingPassFixture`/`MappingPassTest` class boilerplate — an exact copy of the
corresponding section of `test_mapping.cpp`, with no test bodies yet.
`MappingPassFixture` provides `context` (a fresh `MLIRContext` with the
QCO/QTensor/SCF/Arith/Func dialects loaded) and the static helper
`runPass(ModuleOp m, const CompilerTarget& target, const MappingPassOptions& options)`,
which runs `createMappingPass(target, options)` then folds away any resulting
`qco.sink`-of-`qco.swap` idioms via a canonicalizer, returning `LogicalResult`.

**The Bacon-Shor circuit itself.** `find_acceptable_swaps.py` lines 16-21:

    _circ_gen = ftqc_circuits.BaconShorCodeCircuitGenerator()
    x_syndrome_circuit = _circ_gen.x_syndrome_circuit()
    correct_x_circuit  = _circ_gen.correct_x_circuit()
    z_syndrome_circuit = _circ_gen.z_syndrome_circuit()
    correct_z_circuit  = _circ_gen.correct_z_circuit()
    full_circuit = x_syndrome_circuit + correct_x_circuit + z_syndrome_circuit + correct_z_circuit

`ftqc_circuits.BaconShorCodeCircuitGenerator.__init__` allocates
`self.state_qubits` (9 data qubits, built first) and `self.qec_ancillas` (3
ancilla qubits, built second); a fourth group, `self.ftswap_ancillas` (4 more
qubits), is allocated but never used by any of the four circuit methods (see
Surprises & Discoveries), so it is irrelevant here. Two 3x6 integer tables:

    self.Sx = [[0,1,3,4,6,7], [1,2,4,5,7,8], [0,2,3,5,6,8]]
    self.Sz = [[0,3,1,4,2,5], [3,6,4,7,5,8], [0,6,1,7,2,8]]

`x_syndrome_circuit()`: for each of the 3 ancillas, `reset` then `h`; then, for
each ancilla index `control` in 0..2 and each data-qubit index `target` in
`self.Sx[control]` (6 targets per ancilla, 18 CNOTs total), a CNOT
`cx(qec_ancillas[control], state_qubits[target])` (ancilla is CNOT control, data
qubit is CNOT target); then `h` each ancilla again.

`correct_x_circuit()`: three CCZ gates, each on one data qubit and two ancillas:
`ccz(state_qubits[7], qec_ancillas[0], qec_ancillas[1])`,
`ccz(state_qubits[6], qec_ancillas[0], qec_ancillas[2])`,
`ccz(state_qubits[5], qec_ancillas[1], qec_ancillas[2])`. As noted in Surprises
& Discoveries, CCZ's permutation-invariance lets the C++ port use
`builder.mcz({ancilla_a, ancilla_b}, data_qubit)` (ancillas as controls, data
qubit as target) without changing the gate.

`z_syndrome_circuit()`: for each of the 3 ancillas, `reset`; then, for each
ancilla index `target` in 0..2 and each data-qubit index `control` in
`self.Sz[target]` (6 per ancilla, 18 CNOTs total), a CNOT
`cx(state_qubits[control], qec_ancillas[target])` (data qubit is CNOT control,
ancilla is CNOT target — the reversed role from `x_syndrome_circuit`).

`correct_z_circuit()`: three CCX (Toffoli) gates, each with two ancilla controls
and one data-qubit target (operand order significant here, since CCX
distinguishes its target):
`ccx(qec_ancillas[0], qec_ancillas[1], state_qubits[5])`,
`ccx(qec_ancillas[0], qec_ancillas[2], state_qubits[2])`,
`ccx(qec_ancillas[1], qec_ancillas[2], state_qubits[8])`; then a final reset of
each ancilla.

**The target.** The 12-qubit "Rydberg-ion" hardware connectivity graph is
supplied directly by the user in this conversation (it does not appear anywhere
in the repository) as 14 undirected couplings, using 0-based site indices 0
through 11: 0<>1, 1<>2, 2<>3, 2<>4, 3<>4, 4<>5, 5<>6, 5<>7, 6<>7, 7<>8, 8<>9,
8<>10, 9<>10, 10<>11 — containing exactly the three triangles {2,3,4}, {5,6,7},
{8,9,10} (see Surprises & Discoveries).

### Plan of Work

All edits are confined to
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp` (fully
rewritten, replacing the existing stub's content from the
`MappingPassTest`/anonymous-namespace closing brace onward, keeping its existing
header/includes/`getQubitValues`/`MappingPassFixture`/ `MappingPassTest`
preamble unchanged) and
`mlir/unittests/Dialect/QCO/Transforms/Mapping/CMakeLists.txt` (add the new file
to the existing target's source list).

1. In `test_rydberg_ions.cpp`, after the existing `MappingPassTest` class, add a
   *generalized* `isExecutable` helper pair (see Decision Log for why this
   differs from `test_mapping.cpp`'s own, untouched copy): a file-local, static
   `isExecutable` function taking a `Region&`, a
   `DenseMap<Value, CompilerTarget::SiteId>&`, and a `const CompilerTarget&`,
   that walks one region's operations exactly as `test_mapping.cpp`'s version
   does for `StaticOp`/`ResetOp`/`MeasureOp`, but for a `UnitaryOpInterface`
   operation with more than one qubit and not a `BarrierOp`, gathers every input
   qubit's mapped hardware site via `unitaryOp.getInputQubits()` into a small
   vector and requires every pair within that vector to be
   `target.areAdjacent(...)` (after resolving each raw site id through
   `target.vertexForSite(...)`), instead of asserting exactly two qubits and
   checking one pair; and the public entry point
   `static bool isExecutable(func::FuncOp, const CompilerTarget&)`, used as
   `isExecutable(getEntryPoint(m.get()), target)`. Since this test's circuit is
   "flat" (no `scf.for`/`scf.while`/`qco.if`/ `qco.index_switch`), the
   nested-region code paths are not exercised, but omit them entirely rather
   than copying unused, untestable code — this circuit never produces such
   regions, so there is nothing to validate there.

2. Add `static CompilerTarget getRydbergIonTarget()`: returns
   `CompilerTarget(12, couplings, operations)`, where `couplings` is a
   `std::vector<CompilerTarget::Coupling>` literal containing the 14 pairs from
   Context and Orientation, and `operations` is a
   `std::vector<CompilerTarget::Operation>` holding exactly the two entries from
   the Decision Log above (`"ccx"` and `"ccz"`, each width 3) and nothing more.

3. Add an anonymous namespace containing the Bacon-Shor circuit construction,
   split into one function per Python method for direct traceability back to
   `ftqc_circuits.py`:

   - `constexpr std::array<std::array<int64_t, 6>, 3> kSx` and `kSz`,
     transcribed verbatim from the Python `Sx`/`Sz` tables.
   - `struct BaconShorCircuit` with two fields, `SmallVector<Value> dataQubits`
     and `SmallVector<Value> ancillaQubits`, sized to 9 and 3 respectively.
   - `void buildXSyndromeCircuit(QCOProgramBuilder&, BaconShorCircuit&)`,
     `void buildXCorrectionCircuit(QCOProgramBuilder&, BaconShorCircuit&)`,
     `void buildZSyndromeCircuit(QCOProgramBuilder&, BaconShorCircuit&)`, and
     `void buildZCorrectionCircuit(QCOProgramBuilder&, BaconShorCircuit&)`, each
     implementing exactly the corresponding Python method (Context and
     Orientation), using `builder.reset`, `builder.h`, `builder.cx`,
     `builder.mcz`, and `builder.mcx`, always immediately writing each call's
     returned SSA value(s) back into the `dataQubits`/`ancillaQubits` vector
     slots they came from. The CCZ/CCX gates built here stay atomic three-qubit
     `qco.ctrl` operations all the way through routing — no decomposition pass
     is run anywhere in this test.
   - `BaconShorCircuit buildBaconShorCircuit(QCOProgramBuilder& builder)`:
     allocates the 9 data qubits (fixing program indices 0-8), then the 3
     ancilla qubits (fixing program indices 9-11), then calls the four
     `build*Circuit` functions in order X-syndrome, X-correction, Z-syndrome,
     Z-correction, and returns the populated `BaconShorCircuit`. This function
     never measures anything, matching the Python reference exactly.
   - `OwningOpRef<ModuleOp> buildAndFinalizeBaconShorProgram(MLIRContext*)`
     (outside the anonymous namespace containing the functions above, so both
     `TEST_F`s below can call it): calls `buildBaconShorCircuit`, then, purely
     as test instrumentation (see Decision Log and Surprises & Discoveries),
     measures and sinks all 12 qubits and returns `builder.finalize(bits)`.

4. Add
   `TEST_F(RydbergIonMappingPassFixture, MapBaconShorCodeOnRydbergIonTarget)`:
   uses the shared `buildAndFinalizeBaconShorProgram(MLIRContext*)` helper (item
   3, below) — which builds the target-agnostic circuit via
   `buildBaconShorCircuit(builder)`, then, purely as test instrumentation (see
   Decision Log and Surprises & Discoveries: without it, the routed circuit is
   entirely eliminated as dead code before any assertion runs), measures and
   sinks all 12 qubits and calls `builder.finalize(bits)` — to get a finalized
   module; asserts `succeeded(verify(*m))`. Then builds the label string
   `std::string(9, 'B') + std::string(3, 'A')` and calls the fixture's `runPass`
   directly (no decomposition pass at all) with
   `MappingPassOptions{.ntrials = 1, .qubitTypeLabels = qubitTypeLabels}`,
   asserting success, module re-verification,
   `isExecutable(getEntryPoint(m.get()), target)`, that at least one `SWAPOp`
   was inserted (`EXPECT_GT`), and that every `CtrlOp` with two controls (the
   CCX/CCZ shape — `builder.cx`/`cz` are themselves one-control `CtrlOp`s, so
   this must filter by control count, not walk every `CtrlOp`; see Surprises &
   Discoveries) still has exactly 3 qubits and there are exactly 6 of them
   (proving none were decomposed).

5. Add
   `TEST_F(RydbergIonMappingPassFixture, MapBaconShorCodeOnRydbergIonTargetWithDefaultCost)`:
   identical setup, calling `runPass` with `MappingPassOptions{.ntrials = 1}`
   (i.e. `qubitTypeLabels` left at its default empty string), asserting success,
   verification, and `isExecutable` — proving the same circuit still compiles
   through the ordinary (non-labeled) path.

6. In `mlir/unittests/Dialect/QCO/Transforms/Mapping/CMakeLists.txt`, change
   `add_executable(${target_name} test_mapping.cpp)` to
   `add_executable(${target_name} test_mapping.cpp test_rydberg_ions.cpp)`; no
   other line needs to change, since every library the new file needs
   (`MLIRParser`, `MQTCompilerTarget`, `MLIRQCOProgramBuilder`,
   `MLIRQTensorUtils`, `MLIRQCOTransforms`, `MLIRSupportMQT`,
   `GTest::gtest_main`) is already linked for `test_mapping.cpp`.

### Concrete Steps

All commands run from the repository root.

1. Confirm `.agent/plans/native-multi-qubit-gate-routing.md`'s own Concrete
   Steps have been run successfully first (its Progress checklist fully checked)
   — this plan's target/tests will not compile or pass without that capability
   in place.
2. If `build/release` is not already configured for this checkout, configure it
   once: `./.agent/run.sh cmake --preset release`. If it was configured from a
   different checkout path, delete and recreate it:
   `rm -rf build/release && ./.agent/run.sh cmake --preset release`.
3. After editing `CMakeLists.txt` and `test_rydberg_ions.cpp`, reconfigure
   (needed once, since a new source file was added to an existing target) and
   build the focused target: `./.agent/run.sh cmake --preset release` then
   `./.agent/run.sh cmake --build --preset release --target mqt-core-mlir-unittest-mapping`.
4. Run just the two new tests while iterating, from the built binary at
   `./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping`,
   passing `--gtest_filter='RydbergIonMappingPassFixture.MapBaconShorCode*'`
   (note the `RydbergIonMappingPassFixture` prefix, not plain
   `MappingPassFixture` — see Decision Log for why this file's fixture class
   needed a distinct name). Expected on success:
   `[==========] 2 tests ... 2 PASSED`.
5. Once the new tests pass, run the full focused mapping suite with no filter to
   confirm no regression in `test_mapping.cpp`'s existing and router-plan-added
   cases:
   `./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping`
6. Run the full C++ suite once before declaring done:
   `./.agent/run.sh ctest --preset release` Expected: 100% pass, matching the
   pre-existing pass/skip counts (2 QDMI skips) plus this task's 2 new tests and
   the router plan's 3 new tests.
7. Run `./.agent/run.sh uvx nox -s lint` and address any findings.

### Validation and Acceptance

- `MapBaconShorCodeOnRydbergIonTarget` fails to compile before this change and,
  after it, passes, demonstrating that the full 9-data/3-ancilla Bacon-Shor QEC
  circuit — built gate-for-gate from the Python reference — compiles through
  `MappingPass` for the user-specified 12-qubit Rydberg-ion coupling graph with
  the state-dependent `qubit-type-labels` heuristic engaged, its CCZ/CCX gates
  remain genuinely native (undecomposed) three-qubit operations placed on
  mutually adjacent hardware triples, and the fully routed program is
  structurally valid end-to-end (`isExecutable`), with at least one `qco.swap`
  actually inserted (proving the topology's sparsity genuinely required
  routing).
- `MapBaconShorCodeOnRydbergIonTargetWithDefaultCost` demonstrates the same
  circuit and target still compile with `qubit-type-labels` left unset.
- All previously passing tests in `test_mapping.cpp` (both pre-existing ones and
  the three new ones added by `.agent/plans/native-multi-qubit-gate-routing.md`)
  continue to pass unmodified, demonstrating this change is purely additive.
- `./.agent/run.sh uvx nox -s lint` passes on the new/changed files.

### Idempotence and Recovery

Every step is a source edit plus a rebuild/rerun; none mutate persistent state
outside the working tree and the `build/` directory (git-ignored, safe to delete
and reconfigure from scratch via the commands in Concrete Steps — no source or
history is at risk). No destructive git operations are part of this plan. If
`test_rydberg_ions.cpp` fails to compile partway through iteration, the safe
recovery is simply to keep editing that one file and rebuilding the single
focused CMake target named in Concrete Steps; nothing else needs to be reverted.

### Artifacts and Notes

- Source circuit definition: `find_acceptable_swaps.py` (lines 16-21),
  `circuits.py` (the generic `StructureCircuit` builder framework — informative
  context, not itself translated into C++), `ftqc_circuits.py` (the concrete
  `BaconShorCodeCircuitGenerator`, fully transcribed above).
- Dependency: `.agent/plans/native-multi-qubit-gate-routing.md` (the native
  multi-qubit-gate routing capability this plan's target/tests require).
- Prior, unmodified feature this test also exercises:
  `.agent/plans/state-dependent-ab-swap-heuristic.md` (`qubit-type-labels`).

### Interfaces and Dependencies

- New file
  `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp`: extends
  the existing stub (renaming its `MappingPassFixture`/`MappingPassTest` classes
  to `RydbergIonMappingPassFixture`/`RydbergIonMappingPassTest` — see Decision
  Log) with a generalized `isExecutable` helper pair, a `getRydbergIonTarget()`
  helper, a `BaconShorCircuit` struct plus four `build*Circuit` functions, one
  `buildBaconShorCircuit` entry point, one `buildAndFinalizeBaconShorProgram`
  test-instrumentation wrapper, and two
  `TEST_F(RydbergIonMappingPassFixture, ...)` cases, as detailed in Plan of
  Work.
- Modified file `mlir/unittests/Dialect/QCO/Transforms/Mapping/CMakeLists.txt`:
  the `add_executable` source list gains `test_rydberg_ions.cpp`.
- Consumes, unmodified (beyond what
  `.agent/plans/native-multi-qubit-gate-routing.md` already delivers):
  `mlir::qco::createMappingPass(const CompilerTarget&, MappingPassOptions)` and
  `MappingPassOptions::qubitTypeLabels`
  (`mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h`,
  `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`);
  `mlir::qco::QCOProgramBuilder`
  (`mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`);
  `mlir::CompilerTarget`, `mlir::CompilerTarget::Coupling`, and
  `mlir::CompilerTarget::Operation` (`mlir/include/mlir/Compiler/Target.h`).

When you revise this ExecPlan, ensure the change is reflected across all
relevant sections above, and add a note here describing what changed and why.
This version (2026-08-12, third revision) trims the router-generalization design
out into its own ExecPlan, `.agent/plans/native-multi-qubit-gate-routing.md`,
per explicit user request, leaving this plan focused solely on the Bacon-Shor
circuit/test that depends on it.
