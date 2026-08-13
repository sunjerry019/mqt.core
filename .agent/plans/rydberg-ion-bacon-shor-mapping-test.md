## Compile the Bacon-Shor QEC circuit onto a 12-qubit Rydberg-ion target with native CCZ/CCX and the stateful A/B swap heuristic

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

This ExecPlan depends on, and must not begin until, the checked-in ExecPlan
`.agent/plans/native-multi-qubit-gate-routing.md` is complete. That plan
teaches `MappingPass` (the `place-and-route` MLIR pass) to accept and
dynamically SWAP-route a native multi-qubit gate (for example a
doubly-controlled-Z, "CCZ", or a Toffoli/doubly-controlled-X, "CCX") without
requiring it to be decomposed first, whenever the target `CompilerTarget`
explicitly declares that operation (by name and exact qubit count) in its
`operations` capability list; every target that does not declare such an
operation keeps the pass's prior, unconditional "decompose first" behavior.
This plan does not repeat that plan's internal design (which files inside
`Mapping.cpp`/`Target.cpp` change, or why); read
`.agent/plans/native-multi-qubit-gate-routing.md` for that. This plan only
covers using that already-delivered capability to compile one specific,
realistic circuit.

### Purpose / Big Picture

A stand-alone Python research prototype at the repository root
(`find_acceptable_swaps.py`, `circuits.py`, `ftqc_circuits.py` — analysis
scripts, not part of the `mqt-core` Python package, not imported by any
package code) defines a 9-data-qubit "Bacon-Shor" quantum-error-correction
(QEC) circuit with 3 syndrome-extraction ancilla qubits (12 qubits total).
The user separately supplied, directly in this conversation, a 12-qubit
"Rydberg-ion" hardware connectivity graph (not present anywhere in the
repository) whose coupling graph contains exactly three triangles (three
mutually-adjacent hardware sites) — sized and shaped to match this exact
circuit's 12 qubits and its six CCZ/CCX correction gates.

After this change, a new C++ GoogleTest file,
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp`,
reconstructs that same Bacon-Shor circuit gate-for-gate using the MLIR QCO
dialect's `QCOProgramBuilder` C++ API, and compiles it for a `CompilerTarget`
describing the 12-qubit Rydberg-ion connectivity — with that target
explicitly declaring `"ccx"`/`"ccz"` at 3 qubits as native operations, per
the capability delivered by `.agent/plans/native-multi-qubit-gate-routing.md`
— using `MappingPass` with `qubit-type-labels` set so the 9 data qubits are
labeled `B` and the 3 ancilla qubits are labeled `A` (the already-shipped
stateful A/B swap heuristic from commit `388e58ab`, see
`.agent/plans/state-dependent-ab-swap-heuristic.md`). The observable outcome:
building and running the new test binary
(`mqt-core-mlir-unittest-mapping`, the same binary `test_mapping.cpp` already
builds into) produces two new passing GoogleTest cases proving the routed
circuit is fully valid on the given hardware graph — every two-qubit CNOT
sits on two adjacent sites, and every three-qubit CCZ/CCX sits on three
mutually adjacent sites (a triangle) — while every CCZ/CCX gate remains
exactly the atomic three-qubit gate the circuit started with (no
decomposition ever happens), and that the same circuit still compiles when
`qubit-type-labels` is left at its default (unset).

### Progress

- [x] (2026-08-12T00:00Z) Read `AGENTS.md`, `.agent/plans/state-dependent-ab-swap-heuristic.md`,
      `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` in
      full, the existing stub at
      `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp`,
      `find_acceptable_swaps.py`, `circuits.py`, and `ftqc_circuits.py` (to
      extract the exact Bacon-Shor gate sequence), and the `QCOProgramBuilder`
      C++ header to confirm builder method names/signatures
      (`allocQubit`, `reset`, `h`, `cx`, `mcz`, `mcx`, `barrier`, `measure`,
      `sink`, `finalize`).
- [x] (2026-08-12T00:10Z) Wrote and had the user reject a first draft of this
      plan that assumed CCZ/CCX would be decomposed via the
      `decompose-multi-controlled` pass before routing. The user corrected
      this: on this Rydberg-ion architecture CCZ/CCX are native three-qubit
      gates and must not be decomposed; the pass only needs to ensure their
      three qubits end up mutually connected.
- [x] (2026-08-12T00:45Z) Split the router-generalization design (which parts
      of `Mapping.cpp`/`Target.cpp` change, and the `hasExplicitOperations()`
      backward-compatibility analysis) out into its own ExecPlan,
      `.agent/plans/native-multi-qubit-gate-routing.md`, at the user's
      explicit request, so this plan can stay focused purely on the
      Bacon-Shor circuit/test. See that plan for the router design.
- [ ] Confirm `.agent/plans/native-multi-qubit-gate-routing.md` is complete
      (its own Progress checklist fully checked, its own Concrete Steps run
      successfully) before starting any work below — this plan's
      `getRydbergIonTarget()` (Plan of Work item 2) and both new tests
      (Plan of Work item 4) depend directly on that capability existing.
- [ ] Write `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp`
      per Plan of Work below.
- [ ] Add `test_rydberg_ions.cpp` as a second source file to the existing
      `mqt-core-mlir-unittest-mapping` target in
      `mlir/unittests/Dialect/QCO/Transforms/Mapping/CMakeLists.txt`.
- [ ] Build the focused mapping unittest binary and iterate until both new
      tests pass (Concrete Steps).
- [ ] Run the full focused mapping suite (no filter) to confirm no
      regression in the pre-existing `test_mapping.cpp` cases (including the
      new native-multi-qubit-gate tests added by the router plan).
- [ ] Run `./.agent/run.sh uvx nox -s lint`.
- [ ] Final read-through of the diff against `AGENTS.md` and this plan; fill
      in `Outcomes & Retrospective`.

### Surprises & Discoveries

- Observation: `QCOProgramBuilder` has no `ccx`/`ccz` methods; three-qubit
  Toffoli/CCZ gates are instead built via the macro-generated
  `mc##OP_NAME(ValueRange controls, Value target)` family (e.g. `mcx`, `mcz`),
  declared once per base gate by the `DECLARE_ONE_TARGET_ZERO_PARAMETER` macro
  in `mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h` (visible via
  `grep -n "mc##OP_NAME" mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`).
  `mcz({a, b}, c)` realizes a CCZ gate; since CCZ's unitary
  (`diag(1,1,1,1,1,1,1,-1)`) is symmetric under permutation of all three
  qubits, which two of the three become "controls" and which becomes "target"
  in the builder call does not change the gate realized, so it is safe to
  always pass the two ancilla qubits as controls and the data qubit as target
  for the X-correction step even though the Python reference's `ccz(...)`
  argument order puts the data qubit first.
- Observation: `MappingPassOptions` is a C++20 aggregate whose fields must be
  initialized via designated initializers *in the struct's declaration order*
  (the order the corresponding `Option<...>` entries appear in `MappingPass`'s
  `options` list in `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`).
  That order is `nlookahead, alpha, lambda, niterations, ntrials, seed,
  qubitTypeLabels`, so a designated-initializer literal must write `.ntrials
  = ..., .qubitTypeLabels = ...` in that relative order, not the reverse, or
  the file fails to compile.
- Observation: the 12 qubits used across the four Bacon-Shor sub-circuits
  (`x_syndrome_circuit`, `correct_x_circuit`, `z_syndrome_circuit`,
  `correct_z_circuit` in `ftqc_circuits.py`) are exactly `state_qubits[0..8]`
  (9 data qubits) and `qec_ancillas[0..2]` (3 ancilla qubits); the fourth
  qubit group, `ftswap_ancillas` (4 more qubits), is never referenced by any
  of those four methods and is therefore irrelevant to the circuit this task
  builds. Evidence: `grep -n "ftswap_ancillas" ftqc_circuits.py` matches only
  the `__init__` assignment, no other use in the file.
- Observation: listing every triangle (three sites where all three possible
  pairs are couplings) in the user-supplied 12-qubit Rydberg-ion graph
  (0<>1, 1<>2, 2<>3, 2<>4, 3<>4, 4<>5, 5<>6, 5<>7, 6<>7, 7<>8, 8<>9, 8<>10,
  9<>10, 10<>11) finds exactly three: {2,3,4}, {5,6,7}, {8,9,10} — matching
  the circuit's 3 ancilla qubits and 6 CCZ/CCX gates (2 corrections per
  ancilla pairing) closely enough that this graph was evidently designed
  specifically as three native-three-qubit-gate zones connected by an
  otherwise plain linear chain (0-1-2, 4-5, 7-8, 10-11).

### Decision Log

- Decision: split the router-generalization work into its own ExecPlan,
  `.agent/plans/native-multi-qubit-gate-routing.md`, referenced here rather
  than duplicated, per explicit user request. Rationale: keeps "teach the
  router to handle native multi-qubit gates in general" (proven with small,
  synthetic, hand-traceable tests) and "use that capability to compile one
  large, realistic circuit end-to-end" as independently readable,
  independently landable units, matching `.agent/PLANS.md`'s "one ExecPlan
  per independently implemented task" guidance. Date/Author: 2026-08-12,
  implementing agent.
- Decision: build both the CCZ and CCX gates in this test circuit using
  `builder.mcz`/`builder.mcx` (the `qco.ctrl`-based shape the router plan
  teaches `MappingPass`/`CompilerTarget::supports` to recognize as
  `"ccz"`/`"ccx"`), not the pre-existing, already-atomic `RCCXOp`
  (`qco.rccx`, a "relative-phase" Toffoli) for the CCX gates. Rationale:
  `RCCXOp` implements a *different*, cheaper unitary (correct only up to an
  uncorrected phase on some computational basis states) — using it would
  silently change the quantum semantics of the ported Bacon-Shor circuit
  relative to the Python reference's exact `ccx(...)`/`ccz(...)` calls, which
  this task should not introduce silently. There is also no `RCCZOp` at all
  (confirmed by grep of `QCOOps.td`), so the CCZ gates need the
  `builder.mcz`/`qco.ctrl` path regardless; using the same path uniformly for
  both CCX and CCZ keeps the circuit construction symmetric and easy to
  verify against the Python reference gate-for-gate. Date/Author:
  2026-08-12, implementing agent.
- Decision: give `getRydbergIonTarget()`'s `CompilerTarget` an explicit
  `operations` list containing exactly `{CompilerTarget::Operation("ccx", 3,
  0), CompilerTarget::Operation("ccz", 3, 0)}` — nothing else (no `"cx"`,
  `"h"`, `"reset"`, etc.). Rationale: per
  `.agent/plans/native-multi-qubit-gate-routing.md`'s design, `Mapping.cpp`
  only ever consults `target->supports(...)` for gates wider than two
  qubits; every one- and two-qubit gate in this circuit (Hadamards, resets,
  the 36 CNOTs, and the SWAPs the pass itself inserts) routes exactly as it
  does for every other test target in this file, regardless of what the
  `operations` list does or does not mention. Declaring only the two
  operations this test actually needs is therefore both sufficient and the
  minimal, most legible expression of "this hardware's two native
  three-qubit gates are CCX and CCZ." Date/Author: 2026-08-12, implementing
  agent.
- Decision: duplicate (rather than share via a new common header) the
  `isExecutable`/`getQubitValues` static helper functions from
  `test_mapping.cpp` into `test_rydberg_ions.cpp`, but *generalize* this
  file's copy of `isExecutable` to check all-pairs mutual adjacency for a
  wider-than-two-qubit gate (gathering every mapped hardware site via
  `unitaryOp.getInputQubits()` and requiring every pair among them to be
  adjacent) instead of `test_mapping.cpp`'s original, which asserts
  `unitaryOp.getNumQubits() <= 2` and checks exactly one pair. Rationale:
  both are anonymous/`static`-linkage helpers scoped to their own
  translation unit, so duplicating them across the two `.cpp` files linked
  into one binary is not an ODR violation; `test_mapping.cpp`'s own copy is
  left completely untouched (its own tests never exercise a wider-than-two
  gate, so its existing `<= 2` assertion remains valid and this task should
  not touch a file it does not need to touch), while this file's copy needs
  the generalized version to correctly validate this circuit's CCZ/CCX
  gates. Date/Author: 2026-08-12, implementing agent.
- Decision: label program qubits by allocation order — the 9 data qubits
  first (indices 0-8, labeled `B`), then the 3 ancilla qubits (indices 9-11,
  labeled `A`) — matching `BaconShorCodeCircuitGenerator.__init__`'s
  `state_qubits` (built first) then `qec_ancillas` (built second) order, and
  matching how `qubit-type-labels` indexes program qubits by permanent
  allocation order (see `.agent/plans/state-dependent-ab-swap-heuristic.md`'s
  Decision Log). Date/Author: 2026-08-12, implementing agent.
- Decision: do not attempt to prove, inside this test, that the
  state-dependent heuristic produces a measurably different SWAP sequence
  than the default flat-cost heuristic for this specific, large (~90-gate)
  circuit/target pair; that claim is already covered by `test_mapping.cpp`'s
  existing, small, hand-verified `StatefulSwapLabels*` tests. This test's job
  is the separate integration claim that the already-proven heuristic and
  the native-multi-qubit-gate routing capability can be used *together* to
  compile a real, motivating circuit end-to-end into a fully valid routed
  program. Date/Author: 2026-08-12, implementing agent.
- Decision: insert `builder.barrier(...)` over all 12 qubits immediately
  before the final measurements, matching the convention already used by
  `test_mapping.cpp`'s `MapFlatGHZ`/`MapGroverLike`/`MapParallelLoops` tests.
  Rationale: consistency with existing test style in this directory; it also
  additionally exercises `MappingPass`'s handling of a full-width (12-qubit)
  barrier on this specific target, which the pass must treat as exempt from
  the adjacency requirement (confirmed by reading `isExecutable`'s
  `!isa<BarrierOp>(op)` exemption). Date/Author: 2026-08-12, implementing
  agent.

### Outcomes & Retrospective

Not yet started; implementation begins once
`.agent/plans/native-multi-qubit-gate-routing.md` is complete. This section
will be filled in once the new tests build and pass.

### Context and Orientation

**MappingPass and the capability this plan depends on.** See
`.agent/plans/native-multi-qubit-gate-routing.md`'s Context and Orientation
for the full internal design; the summary needed here is: a
`CompilerTarget` (`mlir/include/mlir/Compiler/Target.h`) can pass an explicit
`operations` list (a `std::vector<CompilerTarget::Operation>`, each bundling
a name, a fixed qubit arity, and a parameter count) to its constructor; if it
explicitly lists an operation named e.g. `"ccx"` with `numQubits = 3`, then
`MappingPass` (`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`) accepts
a matching, undecomposed three-qubit gate — built via
`QCOProgramBuilder::mcx`/`mcz` (see below) — and routes it by inserting
ordinary two-qubit SWAPs, if necessary, until its three qubits sit on three
mutually-adjacent hardware sites (a "triangle" in the target's coupling
graph). A target that omits `operations` (or that declares one that does not
include `"ccx"`/`"ccz"` at width 3) keeps the pass's prior, unconditional
behavior: any gate wider than two qubits is rejected with a "decompose it to
one- and two-qubit operations first" diagnostic.

**Builder API.** Programs are constructed in C++ using `QCOProgramBuilder`
(`mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`). It follows
"value semantics": each gate-applying method consumes one or more current
qubit SSA ("Static Single Assignment" — each variable is written exactly
once) values and returns new SSA values representing the qubits' state after
the gate, so call sites look like `q = builder.h(q);` or `std::tie(q0, q1) =
builder.cx(q0, q1);`. Relevant methods: `Value allocQubit()` (allocates one
fresh qubit initialized to `|0>`, implicitly assigning it the next unused
"program index" — the identity `qubit-type-labels` indexes by, in the order
`allocQubit()` is called); `Value reset(Value qubit)`; `Value h(Value
qubit)`; `std::pair<Value, Value> cx(Value control, Value target)`
(controlled-X/CNOT); `std::pair<ValueRange, Value> mcx(ValueRange controls,
Value target)` and `std::pair<ValueRange, Value> mcz(ValueRange controls,
Value target)` (multi-controlled X/Z; with exactly two controls these realize
Toffoli/CCX and doubly-controlled-Z/CCZ respectively — there is no separate
three-qubit-native `ccx`/`ccz` method, these `mc*` methods are the only way
to build one); `ValueRange barrier(ValueRange qubits)`; `std::pair<Value,
Value> measure(Value qubit)` (returns the post-measurement qubit and a
classical `i1` bit); `QCOProgramBuilder& sink(Value qubit)` (marks a qubit's
SSA value consumed for the last time, required by QCO's "linear typing"
discipline before `finalize()`); `void initialize(TypeRange returnTypes)`
(must be called once first, with one `i1` return type per qubit this program
will measure — here 12); and `OwningOpRef<ModuleOp>
finalize(ValueRange returnValues)`.

**Test infrastructure.** Tests live in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/`, built as CMake target
`mqt-core-mlir-unittest-mapping` (`CMakeLists.txt` in that directory
currently lists only `test_mapping.cpp` as a source). An untracked stub,
`test_rydberg_ions.cpp`, already exists in that directory (visible in `git
status`) containing only the shared license header, includes, `using
namespace` declarations, the `getQubitValues` helper, and the
`MappingPassFixture`/`MappingPassTest` class boilerplate — an exact copy of
the corresponding section of `test_mapping.cpp`, with no test bodies yet.
`MappingPassFixture` provides `context` (a fresh `MLIRContext` with the
QCO/QTensor/SCF/Arith/Func dialects loaded) and the static helper
`runPass(ModuleOp m, const CompilerTarget& target, const MappingPassOptions&
options)`, which runs `createMappingPass(target, options)` then folds away
any resulting `qco.sink`-of-`qco.swap` idioms via a canonicalizer, returning
`LogicalResult`.

**The Bacon-Shor circuit itself.** `find_acceptable_swaps.py` lines 16-21:

    _circ_gen = ftqc_circuits.BaconShorCodeCircuitGenerator()
    x_syndrome_circuit = _circ_gen.x_syndrome_circuit()
    correct_x_circuit  = _circ_gen.correct_x_circuit()
    z_syndrome_circuit = _circ_gen.z_syndrome_circuit()
    correct_z_circuit  = _circ_gen.correct_z_circuit()
    full_circuit = x_syndrome_circuit + correct_x_circuit + z_syndrome_circuit + correct_z_circuit

`ftqc_circuits.BaconShorCodeCircuitGenerator.__init__` allocates
`self.state_qubits` (9 data qubits, built first) and `self.qec_ancillas` (3
ancilla qubits, built second); a fourth group, `self.ftswap_ancillas` (4
more qubits), is allocated but never used by any of the four circuit methods
(see Surprises & Discoveries), so it is irrelevant here. Two 3x6 integer
tables:

    self.Sx = [[0,1,3,4,6,7], [1,2,4,5,7,8], [0,2,3,5,6,8]]
    self.Sz = [[0,3,1,4,2,5], [3,6,4,7,5,8], [0,6,1,7,2,8]]

`x_syndrome_circuit()`: for each of the 3 ancillas, `reset` then `h`; then,
for each ancilla index `control` in 0..2 and each data-qubit index `target`
in `self.Sx[control]` (6 targets per ancilla, 18 CNOTs total), a CNOT
`cx(qec_ancillas[control], state_qubits[target])` (ancilla is CNOT control,
data qubit is CNOT target); then `h` each ancilla again.

`correct_x_circuit()`: three CCZ gates, each on one data qubit and two
ancillas: `ccz(state_qubits[7], qec_ancillas[0], qec_ancillas[1])`,
`ccz(state_qubits[6], qec_ancillas[0], qec_ancillas[2])`,
`ccz(state_qubits[5], qec_ancillas[1], qec_ancillas[2])`. As noted in
Surprises & Discoveries, CCZ's permutation-invariance lets the C++ port use
`builder.mcz({ancilla_a, ancilla_b}, data_qubit)` (ancillas as controls, data
qubit as target) without changing the gate.

`z_syndrome_circuit()`: for each of the 3 ancillas, `reset`; then, for each
ancilla index `target` in 0..2 and each data-qubit index `control` in
`self.Sz[target]` (6 per ancilla, 18 CNOTs total), a CNOT
`cx(state_qubits[control], qec_ancillas[target])` (data qubit is CNOT
control, ancilla is CNOT target — the reversed role from `x_syndrome_circuit`).

`correct_z_circuit()`: three CCX (Toffoli) gates, each with two ancilla
controls and one data-qubit target (operand order significant here, since
CCX distinguishes its target): `ccx(qec_ancillas[0], qec_ancillas[1],
state_qubits[5])`, `ccx(qec_ancillas[0], qec_ancillas[2], state_qubits[2])`,
`ccx(qec_ancillas[1], qec_ancillas[2], state_qubits[8])`; then a final reset
of each ancilla.

**The target.** The 12-qubit "Rydberg-ion" hardware connectivity graph is
supplied directly by the user in this conversation (it does not appear
anywhere in the repository) as 14 undirected couplings, using 0-based site
indices 0 through 11: 0<>1, 1<>2, 2<>3, 2<>4, 3<>4, 4<>5, 5<>6, 5<>7, 6<>7,
7<>8, 8<>9, 8<>10, 9<>10, 10<>11 — containing exactly the three triangles
{2,3,4}, {5,6,7}, {8,9,10} (see Surprises & Discoveries).

### Plan of Work

All edits are confined to
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp` (fully
rewritten, replacing the existing stub's content from the
`MappingPassTest`/anonymous-namespace closing brace onward, keeping its
existing header/includes/`getQubitValues`/`MappingPassFixture`/
`MappingPassTest` preamble unchanged) and
`mlir/unittests/Dialect/QCO/Transforms/Mapping/CMakeLists.txt` (add the new
file to the existing target's source list).

1. In `test_rydberg_ions.cpp`, after the existing `MappingPassTest` class,
   add a *generalized* `isExecutable` helper pair (see Decision Log for why
   this differs from `test_mapping.cpp`'s own, untouched copy): a
   file-local, `static bool isExecutable(Region&, DenseMap<Value,
   CompilerTarget::SiteId>&, const CompilerTarget&)` that walks one region's
   operations exactly as `test_mapping.cpp`'s version does for `StaticOp`/
   `ResetOp`/`MeasureOp`, but for a `UnitaryOpInterface` operation with more
   than one qubit and not a `BarrierOp`, gathers every input qubit's mapped
   hardware site via `unitaryOp.getInputQubits()` into a small vector and
   requires every pair within that vector to be `target.areAdjacent(...)`
   (after resolving each raw site id through `target.vertexForSite(...)`),
   instead of asserting exactly two qubits and checking one pair; and the
   public entry point `static bool isExecutable(func::FuncOp, const
   CompilerTarget&)`, used as `isExecutable(getEntryPoint(m.get()), target)`.
   Since this test's circuit is "flat" (no `scf.for`/`scf.while`/`qco.if`/
   `qco.index_switch`), the nested-region code paths are not exercised, but
   omit them entirely rather than copying unused, untestable code — this
   circuit never produces such regions, so there is nothing to validate
   there.

2. Add `static CompilerTarget getRydbergIonTarget()`: returns
   `CompilerTarget(12, couplings, operations)`, where `couplings` is a
   `std::vector<CompilerTarget::Coupling>` literal containing the 14 pairs
   from Context and Orientation, and `operations` is
   `std::vector<CompilerTarget::Operation>{CompilerTarget::Operation("ccx",
   3, 0), CompilerTarget::Operation("ccz", 3, 0)}` (see Decision Log for why
   exactly these two and nothing more).

3. Add an anonymous namespace containing the Bacon-Shor circuit
   construction, split into one function per Python method for direct
   traceability back to `ftqc_circuits.py`:

   - `constexpr std::array<std::array<int64_t, 6>, 3> kSx` and `kSz`,
     transcribed verbatim from the Python `Sx`/`Sz` tables.
   - `struct BaconShorCircuit { SmallVector<Value> dataQubits;
     SmallVector<Value> ancillaQubits; };` sized to 9 and 3 respectively.
   - `void buildXSyndromeCircuit(QCOProgramBuilder&, BaconShorCircuit&)`,
     `void buildXCorrectionCircuit(QCOProgramBuilder&, BaconShorCircuit&)`,
     `void buildZSyndromeCircuit(QCOProgramBuilder&, BaconShorCircuit&)`, and
     `void buildZCorrectionCircuit(QCOProgramBuilder&, BaconShorCircuit&)`,
     each implementing exactly the corresponding Python method (Context and
     Orientation), using `builder.reset`, `builder.h`, `builder.cx`,
     `builder.mcz`, and `builder.mcx`, always immediately writing each
     call's returned SSA value(s) back into the `dataQubits`/`ancillaQubits`
     vector slots they came from. The CCZ/CCX gates built here stay atomic
     three-qubit `qco.ctrl` operations all the way through routing — no
     decomposition pass is run anywhere in this test.
   - `BaconShorCircuit buildBaconShorCircuit(QCOProgramBuilder& builder)`:
     allocates the 9 data qubits (fixing program indices 0-8), then the 3
     ancilla qubits (fixing program indices 9-11), then calls the four
     `build*Circuit` functions in order X-syndrome, X-correction,
     Z-syndrome, Z-correction, and returns the populated `BaconShorCircuit`.

4. Add `TEST_F(MappingPassFixture, MapBaconShorCodeOnRydbergIonTarget)`:
   builds the target via `getRydbergIonTarget()`; constructs a
   `QCOProgramBuilder`, calls `builder.initialize(SmallVector<Type>(12,
   builder.getI1Type()))`; calls `buildBaconShorCircuit(builder)`; applies
   `builder.barrier(...)` across all 12 qubits (concatenating `dataQubits`
   then `ancillaQubits`, writing the barrier's returned values back); measures
   each of the 12 qubits into a `SmallVector<Value> bits(12)` (data qubits'
   bits at indices 0-8, ancillas' at 9-11); sinks all 12 post-measurement
   values; calls `builder.finalize(bits)`; asserts `succeeded(verify(*m))`.
   Then builds the label string `std::string(9, 'B') + std::string(3, 'A')`
   and calls the fixture's `runPass(m.get(), target,
   MappingPassOptions{.ntrials = 1, .qubitTypeLabels = qubitTypeLabels})`
   directly (no decomposition pass at all), asserting success, module
   re-verification, `isExecutable(getEntryPoint(m.get()), target)`, that at
   least one `SWAPOp` was inserted (`EXPECT_GT`, not `ASSERT`, in case a
   future target/topology change makes it legitimately zero), and that every
   remaining `CtrlOp` still has exactly 3 qubits (proving none were
   decomposed).

5. Add `TEST_F(MappingPassFixture,
   MapBaconShorCodeOnRydbergIonTargetWithDefaultCost)`: identical setup,
   calling `runPass` with `MappingPassOptions{.ntrials = 1}` (i.e.
   `qubitTypeLabels` left at its default empty string), asserting success,
   verification, and `isExecutable` — proving the same circuit still
   compiles through the ordinary (non-labeled) path.

6. In `mlir/unittests/Dialect/QCO/Transforms/Mapping/CMakeLists.txt`, change
   `add_executable(${target_name} test_mapping.cpp)` to
   `add_executable(${target_name} test_mapping.cpp test_rydberg_ions.cpp)`;
   no other line needs to change, since every library the new file needs
   (`MLIRParser`, `MQTCompilerTarget`, `MLIRQCOProgramBuilder`,
   `MLIRQTensorUtils`, `MLIRQCOTransforms`, `MLIRSupportMQT`,
   `GTest::gtest_main`) is already linked for `test_mapping.cpp`.

### Concrete Steps

All commands run from the repository root.

1. Confirm `.agent/plans/native-multi-qubit-gate-routing.md`'s own Concrete
   Steps have been run successfully first (its Progress checklist fully
   checked) — this plan's target/tests will not compile or pass without
   that capability in place.
2. If `build/release` is not already configured for this checkout, configure
   it once: `./.agent/run.sh cmake --preset release`. If it was configured
   from a different checkout path, delete and recreate it:
   `rm -rf build/release && ./.agent/run.sh cmake --preset release`.
3. After editing `CMakeLists.txt` and `test_rydberg_ions.cpp`, reconfigure
   (needed once, since a new source file was added to an existing target)
   and build the focused target:
   `./.agent/run.sh cmake --preset release` then
   `./.agent/run.sh cmake --build --preset release --target mqt-core-mlir-unittest-mapping`.
4. Run just the two new tests while iterating:
   `./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping --gtest_filter='MappingPassFixture.MapBaconShorCode*'`
   Expected on success: `[==========] 2 tests ... 2 PASSED`.
5. Once the new tests pass, run the full focused mapping suite with no
   filter to confirm no regression in `test_mapping.cpp`'s existing and
   router-plan-added cases:
   `./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping`
6. Run the full C++ suite once before declaring done:
   `./.agent/run.sh ctest --preset release`
   Expected: 100% pass, matching the pre-existing pass/skip counts (2 QDMI
   skips) plus this task's 2 new tests and the router plan's 3 new tests.
7. Run `./.agent/run.sh uvx nox -s lint` and address any findings.

### Validation and Acceptance

- `MapBaconShorCodeOnRydbergIonTarget` fails to compile before this change
  and, after it, passes, demonstrating that the full 9-data/3-ancilla
  Bacon-Shor QEC circuit — built gate-for-gate from the Python reference —
  compiles through `MappingPass` for the user-specified 12-qubit Rydberg-ion
  coupling graph with the state-dependent `qubit-type-labels` heuristic
  engaged, its CCZ/CCX gates remain genuinely native (undecomposed)
  three-qubit operations placed on mutually adjacent hardware triples, and
  the fully routed program is structurally valid end-to-end
  (`isExecutable`), with at least one `qco.swap` actually inserted (proving
  the topology's sparsity genuinely required routing).
- `MapBaconShorCodeOnRydbergIonTargetWithDefaultCost` demonstrates the same
  circuit and target still compile with `qubit-type-labels` left unset.
- All previously passing tests in `test_mapping.cpp` (both pre-existing ones
  and the three new ones added by
  `.agent/plans/native-multi-qubit-gate-routing.md`) continue to pass
  unmodified, demonstrating this change is purely additive.
- `./.agent/run.sh uvx nox -s lint` passes on the new/changed files.

### Idempotence and Recovery

Every step is a source edit plus a rebuild/rerun; none mutate persistent
state outside the working tree and the `build/` directory (git-ignored, safe
to delete and reconfigure from scratch via the commands in Concrete Steps —
no source or history is at risk). No destructive git operations are part of
this plan. If `test_rydberg_ions.cpp` fails to compile partway through
iteration, the safe recovery is simply to keep editing that one file and
rebuilding the single focused CMake target named in Concrete Steps; nothing
else needs to be reverted.

### Artifacts and Notes

- Source circuit definition: `find_acceptable_swaps.py` (lines 16-21),
  `circuits.py` (the generic `StructureCircuit` builder framework —
  informative context, not itself translated into C++), `ftqc_circuits.py`
  (the concrete `BaconShorCodeCircuitGenerator`, fully transcribed above).
- Dependency: `.agent/plans/native-multi-qubit-gate-routing.md` (the native
  multi-qubit-gate routing capability this plan's target/tests require).
- Prior, unmodified feature this test also exercises:
  `.agent/plans/state-dependent-ab-swap-heuristic.md` (`qubit-type-labels`).

### Interfaces and Dependencies

- New file `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_rydberg_ions.cpp`:
  extends the existing stub with a generalized `isExecutable` helper pair, a
  `getRydbergIonTarget()` helper, a `BaconShorCircuit` struct plus four
  `build*Circuit` functions and one `buildBaconShorCircuit` entry point, and
  two `TEST_F(MappingPassFixture, ...)` cases, as detailed in Plan of Work.
- Modified file
  `mlir/unittests/Dialect/QCO/Transforms/Mapping/CMakeLists.txt`: the
  `add_executable` source list gains `test_rydberg_ions.cpp`.
- Consumes, unmodified (beyond what
  `.agent/plans/native-multi-qubit-gate-routing.md` already delivers):
  `mlir::qco::createMappingPass(const CompilerTarget&, MappingPassOptions)`
  and `MappingPassOptions::qubitTypeLabels`
  (`mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h`,
  `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`);
  `mlir::qco::QCOProgramBuilder`
  (`mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`);
  `mlir::CompilerTarget`, `mlir::CompilerTarget::Coupling`, and
  `mlir::CompilerTarget::Operation`
  (`mlir/include/mlir/Compiler/Target.h`).

When you revise this ExecPlan, ensure the change is reflected across all
relevant sections above, and add a note here describing what changed and
why. This version (2026-08-12, third revision) trims the router-generalization
design out into its own ExecPlan,
`.agent/plans/native-multi-qubit-gate-routing.md`, per explicit user request,
leaving this plan focused solely on the Bacon-Shor circuit/test that depends
on it.