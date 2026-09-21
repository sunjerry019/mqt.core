# Count the SWAPs that immediately follow a two-qubit gate on the same qubit pair

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

## Purpose / Big Picture

When the compiler inserts a SWAP operation into a quantum circuit, that SWAP may
later have to be built in an expensive protected form rather than a cheap
unprotected one. On the Rydberg-ion hardware the protected form costs roughly
seven and a half times as much. Which SWAPs need protecting is decided outside
this compiler, by a simulator in a separate project, and that decision is slow.

There is one case where the answer is known in advance and for free. If a SWAP
acts on exactly the same pair of qubits that a two-qubit gate of the original
circuit has just acted on, with nothing in between touching either qubit, then
those two qubits were already directly interacting a moment earlier. The SWAP
creates no path for a fault to spread that the gate had not already created, so
the original circuit's own fault tolerance already covers it and the SWAP can
stay cheap.

Before anyone tries to make the router produce more of these, someone has to
know how many it produces today, and that number has to come from many routings
rather than from a handful, because the router's output varies from run to run.
This plan adds nothing but the measurement: a statistic, reported by the pass,
counting these SWAPs. It changes no routing decision whatsoever.

You can see it working by routing the same circuit several times and reading the
new statistic, and by confirming that the routed circuits themselves are
unchanged by this plan.

## Progress

- [ ] Read the orientation section and confirm the named code matches the
      current working tree.
- [ ] Add the per-site tracking of the most recent gates to the routing state.
- [ ] Add the qualifying test and the statistic.
- [ ] Add the two GoogleTests described under Validation and Acceptance.
- [ ] Build, run the mapping tests, and record results in
      `Outcomes & Retrospective`.
- [ ] Measure the statistic across many seeds on the Bacon-Shor circuit and
      record the distribution, not a single number, in
      `Outcomes & Retrospective`.

## Surprises & Discoveries

- Observation: only one of the two possible orderings can ever occur, which
  greatly simplifies both the counting and any future use of it. A SWAP cannot
  be immediately followed by a two-qubit gate on the same site pair. If after a
  SWAP on sites a and b the gate's two qubits occupy a and b, then before the
  SWAP they occupied b and a, which is the same unordered pair, so they were
  already adjacent and the gate was already executable; in that situation the
  search returns without emitting anything. Evidence: scanning four routed
  Bacon-Shor circuits found zero occurrences of a SWAP followed by a gate on the
  same pair, and five, two, two and two occurrences of the reverse ordering.

- Observation: a first attempt at this scan produced zero everywhere because it
  searched forward from each SWAP for a following gate, which as the previous
  point explains can never find anything. The correct scan walks forward from
  every two-qubit gate looking for a following SWAP. Recording this because the
  same mistake is easy to repeat when writing the test.

## Decision Log

- Decision: ship the measurement on its own, before any change to the cost model
  that would use it. Rationale: the value of biasing the router toward these
  SWAPs depends entirely on how many opportunities exist, and today's evidence
  is four routings, which is not enough to justify touching the search's hot
  path. Shipping the statistic first makes the baseline measurable with the same
  instrument that will later measure the improvement, which is the only way the
  comparison is meaningful. Date/Author: 2026-09-18, this plan.

- Decision: define "nothing in between" to include single-qubit gates, so that a
  Hadamard on either qubit between the gate and the SWAP disqualifies the pair.
  Rationale: this is the conservative reading and it matches the definition the
  hardware owner chose; relaxing it later is a safe direction whereas tightening
  it is not. Date/Author: 2026-09-18, this plan, at the user's explicit
  direction.

- Decision: count the qualifying SWAPs during routing, in the code that emits
  them, rather than by scanning the finished circuit afterwards. Rationale: the
  qualifying condition is about what had executed at the moment the SWAP was
  chosen, which the routing loop knows directly and a post-hoc scan of the final
  circuit would have to reconstruct. Date/Author: 2026-09-18, this plan.

## Outcomes & Retrospective

Not yet started. On completion, record the distribution of the new statistic
over at least thirty seeds on the Bacon-Shor circuit: its mean, its spread, and
its range, alongside the distribution of the total SWAP count over the same
seeds. That pair of distributions is the input to the decision about whether
`.agent/plans/gate-adjacent-swap-bias.md` is worth implementing.

## Context and Orientation

Everything below is a fact about the repository as it stands. Verify it.

Terms, in plain language. The *mapping pass* inserts SWAP operations into a
quantum circuit so that every multi-qubit gate acts on hardware sites the
machine directly connects. It is declared in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` as `def MappingPass`,
command-line name `place-and-route`, and implemented as the single `MappingPass`
struct in `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`. A *site* is a
hardware qubit position. A *program qubit* is a qubit of the user's circuit;
which site holds which program qubit changes as SWAPs execute, and that
assignment is the *layout*.

The routing loop works as follows. `advance()`, at approximately line 1438,
executes every gate whose qubits already sit on directly connected sites, and
stops when no more can be executed. `getWindow()`, at approximately line 1343,
collects the gates that are blocked, in layers. `search()`, at approximately
line 1091, runs a search for a sequence of SWAPs that makes the first layer
executable. `insertSWAPs`, at approximately line 1394, writes those SWAPs into
the circuit and records `stats.nswaps += swaps.size();` at approximately line
1431. The loop then returns to `advance()`. Crucially, no gates execute between
the start and the end of one `search()` call, so every SWAP a single search
emits sees the same set of already-executed gates.

`struct Statistics`, at approximately line 160, currently holds one field,
`size_t nswaps{0};`. The pass declares a statistic `numSwaps` with command-line
name `num-inserted-swaps` in the tablegen file, and accumulates it in
`runOnOperation` at approximately line 476 with `numSwaps += stats.nswaps;`.

The pass does not produce identical output across repeated runs of the same
binary with the same inputs and the same seed. The cause is
`mlir/include/mlir/Dialect/QCO/Utils/Drivers.h` line 37, which declares
`using ReadyMap = llvm::SmallDenseMap<Operation*, SmallVector<size_t>, 8>;`, a
map keyed on heap addresses, iterated during routing at `Mapping.cpp` line 1354.
Five consecutive runs of the Rydberg-ion test with identical inputs produced 83,
85, 86, 86 and 93 inserted SWAPs. Every measurement in this plan is therefore a
measurement of a distribution, and no single pair of runs proves anything.

## Plan of Work

In `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`, add two vectors to the
state that the routing loop carries, each indexed by site and holding an
`Operation*`, both sized to the target's site count and initialised to null. The
first records the most recent two-qubit gate to have acted on that site. The
second records the most recent gate of any arity to have acted on that site. Put
them wherever the routing loop's other per-run state lives, and make sure they
are reset for each routing pass, including the refinement passes that
`generateLayout` runs, so that a stale entry from one pass cannot leak into
another.

Update both vectors in `advance()`, at the point where a gate is recorded as
executed. For every site the executed gate touches, set the any-arity entry to
that gate; and if the gate acts on exactly two qubits, also set the two-qubit
entry for both its sites to that gate. Do this for gates of every arity,
including the native three-qubit gates, because a three-qubit gate touching a
site must invalidate that site's pair even though it never establishes one.

Add a predicate, taking a pair of sites and returning whether that pair
qualifies. A pair of sites qualifies when: the most recent two-qubit gate on the
first site is not null; it is the same operation as the most recent two-qubit
gate on the second site; and that same operation is also the most recent gate of
any arity on each of the two sites. The last condition is what enforces "nothing
in between", including single-qubit gates, because any later gate on either site
would have overwritten that site's any-arity entry.

In `insertSWAPs`, for each SWAP being emitted, evaluate the predicate on its
site pair before the SWAP is applied to the layout, and increment a new counter
in `struct Statistics`, for example `size_t nGateAdjacentSwaps{0};`. Take care
with ordering here: within one call, `insertSWAPs` emits a sequence of SWAPs,
and the predicate must be evaluated against the gates that had executed before
the search began. Since no gates execute during a search, the two tracking
vectors do not change during the sequence, so simply evaluating the predicate
per SWAP is correct. Add a comment saying so, because it is the kind of thing a
later reader will suspect is a bug.

In `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`, add a statistic beside
the existing `numSwaps`, named `numGateAdjacentSwaps` with command-line name
`num-gate-adjacent-swaps`, documented as the number of inserted SWAP operations
that acted on exactly the site pair of the immediately preceding two-qubit gate,
with no intervening gate on either site. Accumulate it in `runOnOperation`
alongside `numSwaps += stats.nswaps;`.

Nothing in this plan may change which SWAPs are chosen. If a routed circuit
changes, something has gone wrong.

## Concrete Steps

Work from the repository root at `/Users/yudong/Documents/projects/mqt.core`.

Establish a baseline before editing:

    cmake --preset release
    cmake --build --preset release --target mqt-core-mlir-unittest-mapping
    ./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping

Expect a transcript ending in a line reporting all tests passed; record the
count. After the edits, rebuild and rerun; the count must be identical, because
this plan changes no behaviour.

To observe the statistic, the routing tool from
`.agent/plans/rydberg-ion-routing-tool.md` is strongly preferred, because the
number must be read across many seeds. With that tool built, collect the
statistic for at least thirty seeds and summarise the distribution. If the tool
does not exist yet, the Rydberg-ion unit-test binary at
`./build/release/mlir/unittests/Dialect/QCO/Transforms/RydbergIons/mqt-core-mlir-unittest-rydberg-ions`
exercises one configuration and can be run repeatedly for a rough reading, but
changing its seed requires editing and rebuilding it, so it is not a practical
instrument for thirty samples.

## Validation and Acceptance

Add two GoogleTests to
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`.

The first proves the counter is right on a case whose answer is known by hand.
Construct a target and a circuit small enough that you can trace the routing
yourself, containing a two-qubit gate on a pair of qubits followed by a
situation that forces the router to swap that same pair. Assert that the
statistic reports one. Then insert a single-qubit gate on one of the two qubits
between the gate and the swap, and assert that the statistic reports zero, which
is the case that proves the any-arity condition is actually being enforced
rather than accidentally satisfied.

The second proves the plan changed nothing. Route a circuit before and after is
not possible within one test, so instead assert the property directly: route the
same circuit twice within one process with identical options, and assert that
the reported SWAP count is identical and that the statistic is well defined,
that is no greater than the SWAP count. The stronger guarantee, that routed
output is byte-identical to what the previous commit produced, cannot be
asserted in a test because the output varies between process launches; verify it
manually instead by running the Rydberg-ion binary several times before and
after the change and confirming the SWAP counts fall in the same range.

The behavioural acceptance is a measurement, not a threshold: the statistic must
be collected over at least thirty seeds on the Bacon-Shor circuit and its
distribution recorded in `Outcomes & Retrospective`. There is no pass or fail
number. A finding that the count is consistently small is a legitimate and
useful outcome, and it is the finding that would argue against implementing
`.agent/plans/gate-adjacent-swap-bias.md`.

## Idempotence and Recovery

All steps are repeatable and nothing outside the build directory is modified.

The one way this plan can do harm is if updating the tracking vectors
accidentally changes control flow in `advance()`. Keep the updates to pure
assignment with no early returns and no conditions other than the arity check.
If the mapping tests regress, that is where to look first.

The likeliest correctness bug is forgetting to reset the vectors between routing
passes, because `generateLayout` runs several refinement passes over the same
program before the real one. The symptom is a count that is too high and that
changes when `niterations` or `ntrials` changes even though the final routing
did not. Guard against it by resetting the vectors in the same place the routing
state is otherwise initialised, and by including a test that runs with `ntrials`
greater than one.

## Artifacts and Notes

The reasoning behind the rule, recorded so a later reader need not reconstruct
it. A SWAP is three CZ gates on this hardware, and a protected SWAP is nine,
arranged as three bare SWAPs through an ancilla. Protection is needed when a
single fault on one of the two qubits could spread onto the other and produce a
correlated error of weight two that the code cannot correct. If a two-qubit gate
of the original circuit has just coupled exactly those two qubits, that path
already existed and the original circuit was designed to tolerate it, so the
SWAP adds no new uncorrectable path.

A related circuit rewrite, which is not what this plan does but is worth reading
for the algebra, is `CircuitOptimizer::cancelCNOTs` in
`src/circuit_optimizer/CircuitOptimizer.cpp` at approximately lines 1182 to
1202, which rewrites a controlled-NOT followed by a SWAP on the same pair into
two controlled-NOTs. It belongs to the repository's legacy non-MLIR code path
and is not wired into this pipeline.

## Interfaces and Dependencies

No new libraries. The change is confined to `MLIRQCOTransforms` and its tablegen
declaration.

At the end of this milestone the following must exist. `struct Statistics` in
`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` must carry a counter of
qualifying SWAPs beside `nswaps`. `def MappingPass` in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` must declare a statistic
`numGateAdjacentSwaps` with command-line name `num-gate-adjacent-swaps`. No pass
option is added by this plan, so `MappingPassOptions` is unchanged and every
existing caller compiles untouched.
