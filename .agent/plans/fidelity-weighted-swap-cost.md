# Replace the mapping pass's multiplicative swap cost with a fidelity-weighted cost table

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

## Purpose / Big Picture

The mapping pass inserts SWAP operations to make a program's two-qubit gates
executable on hardware whose qubits are not all directly connected. Each
inserted SWAP costs real fidelity on the machine, and the pass already tries to
prefer cheaper SWAPs over more expensive ones through an opt-in cost model. On
the twelve-site Rydberg-ion trapped-ion target that this repository's
`mlir/unittests/Dialect/QCO/Transforms/RydbergIons/test_rydberg_ions.cpp`
exercises, that cost model is measurably miscalibrated: it cannot represent the
real relative costs, and as a result it ranks two important situations as
equally expensive when in reality one is about twenty-five percent cheaper than
the other.

After this change, a user compiling for that target can supply the two measured
CZ gate fidelities of their hardware and a small table of empirical
fault-tolerance probabilities, and the pass will compute each candidate SWAP's
cost as an expected error rate rather than as an arbitrary product of two
integers. The user-visible outcome is that routing decisions change, and that
the routed program's total expected error, computed from the same fidelities,
goes down relative to the existing model on the same input.

You can see it working by routing the Bacon-Shor circuit twice, once with the
existing cost model and once with the new one, and comparing the reported
expected-error figure printed by the new statistic this plan adds.

## Progress

- [ ] Read the orientation section and confirm the current cost expression at
      `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` lines 215 to 273
      matches what this plan describes.
- [ ] Add the three new pass options to
      `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`.
- [ ] Add option parsing and the precomputed six-entry cost table to the pass.
- [ ] Replace the per-SWAP cost expression with a table lookup.
- [ ] Add the `expectedError` statistic.
- [ ] Add the two GoogleTests described under Validation and Acceptance.
- [ ] Run the build and the tests, and record the observed numbers in
      `Outcomes & Retrospective`.

## Surprises & Discoveries

- Observation: the existing multiplicative cost model cannot fit the measured
  costs for structural reasons, not merely because its constants are wrong.
  Evidence: the real ratio between the cost of a SWAP on a
  next-nearest-neighbour edge and the same SWAP on a nearest-neighbour edge is
  5.2 when both program qubits are auxiliary, 3.4 when one is auxiliary and one
  is data, and 3.0 when both are data. A single multiplier, which is what the
  current `nnnCostMultiplier` option provides, forces one ratio for all three
  cases and therefore cannot match any two of them simultaneously.

- Observation: a fault-tolerant SWAP is not built purely from nearest-neighbour
  operations, which makes fault-tolerant SWAPs considerably more expensive than
  a naive gate count suggests. Evidence: on this target the three sites involved
  in a fault-tolerant SWAP form a triangle whose closing edge is a
  next-nearest-neighbour edge, so one of the three constituent bare SWAPs
  necessarily crosses that more error-prone edge. Confirmed by the hardware
  owner on 2026-09-23; until then it was an inference from the target's
  geometry.

- Observation: the two hand-compiled reference circuits in the sibling
  repository's `scripts/manual_compilations.py` never place a SWAP on a
  diagonal. Every one of their 90 and 77 SWAPs runs along the chain, and the
  diagonals (2,4), (5,7) and (8,10) appear only under two-qubit gates, where the
  circuit's own comments name them explicitly ("over the (2,4) diagonal"). The
  router does not follow that rule: `4.in` puts 27 of its 76 SWAPs on diagonals
  and `5.in` 2 of 84. Pricing each SWAP by the connection it crosses rather than
  flat, the four circuits cost 503.0, 307.0, 300.0 and 240.9 in units of a bare
  nearest-neighbour SWAP, against flat figures of 251.5, 285.5, 298.0 and
  239.5 --- so the edge mix alone doubles `4.in` and leaves the other three
  almost unchanged. This is the most direct evidence yet that the edge-aware
  half of this plan matters, and it suggests a sharper rule than a multiplier:
  an expert uses a diagonal for a gate and never for a SWAP.

## Decision Log

- Decision: keep the new cost model opt-in and default-disabled, leaving the
  existing multiplicative behaviour byte-for-byte unchanged when the new options
  are not supplied. Rationale: the two cost heuristics already present in this
  pass, selected by the `qubitTypeLabels` and `nnnEdges` options, were both
  shipped as strictly additive opt-in features whose empty defaults reproduce
  prior behaviour exactly; following that precedent keeps every existing test
  valid and makes the change safe to land independently. Date/Author:
  2026-09-18, this plan.

- Decision: express the new model in terms of two measured gate fidelities and
  three measured probabilities rather than as six opaque cost constants.
  Rationale: fidelities are quantities a hardware operator can measure and
  report, and the probabilities can be re-derived from simulation output, so the
  model stays honest as the hardware or the circuit family changes, whereas six
  tuned constants would silently rot. Date/Author: 2026-09-18, this plan.

- Decision: keep the three fault-tolerance probabilities as an option string
  rather than promoting the currently-measured values to defaults. Rationale:
  besides the honesty argument above, the evaluation campaign described in
  `/Users/yudong/Documents/projects/qec-rydberg-ions/.agent/plans/evaluation-protocol.md`
  treats "hand-derived cost table" and "cost table measured from many routings"
  as two settings of the same knob, to be compared against each other and
  against the simpler integer model. That comparison is only possible if the
  numbers are an input to the pass; baking any particular set of them in would
  collapse two experimental conditions into one. Date/Author: 2026-09-18, this
  plan.

- Decision: precompute the six costs once per pass invocation into a fixed-size
  array indexed by edge kind and label pair, rather than evaluating the
  expectation formula per candidate SWAP. Rationale: the A* search evaluates
  this cost many thousands of times per routing, and the inputs are constant for
  the whole pass, so the arithmetic must not sit on the hot path; the resulting
  lookup is the same cost as today's multiplication. Date/Author: 2026-09-18,
  this plan.

## Outcomes & Retrospective

Not yet started. On completion, record here the routed swap counts and the
expected-error statistic for the Bacon-Shor test circuit under both cost models,
and state whether the new model reduced expected error as intended.

## Context and Orientation

Everything in this section is a fact about the repository as it stands today.
Verify each claim before relying on it.

The pass in question is declared in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` as `def MappingPass`, whose
command-line name is `place-and-route`. Its implementation is the single
`MappingPass` struct in `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`.
Its factory function is
`createMappingPass(const CompilerTarget& target, MappingPassOptions options)`,
declared in `mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h`.

Some terms used throughout, defined in plain language:

A *target* is a description of the hardware: how many qubit sites it has, which
pairs of sites are directly connected, and which operations it can perform
natively. It is the `CompilerTarget` class in
`mlir/include/mlir/Compiler/Target.h`.

A *coupling edge* is a pair of sites the target says are directly connected.

On the Rydberg-ion target, coupling edges come in two physical kinds even though
the target treats them identically. A *nearest-neighbour edge*, abbreviated NN,
connects two ions adjacent in the trap. A *next-nearest-neighbour edge*,
abbreviated NNN, connects two ions with one ion between them. The pass learns
which edges are NNN from the existing `nnnEdges` option, a comma-separated list
of site pairs such as `2-4,5-7,8-10`.

A *program qubit* is a qubit in the user's circuit, as opposed to a hardware
site. The pass assigns program qubits to sites and rewrites the circuit so that
every two-qubit gate acts on directly connected sites, inserting SWAP operations
to move program qubits around.

A *label* is an opt-in per-program-qubit annotation supplied through the
existing `qubitTypeLabels` option, a string of characters where `A` means the
program qubit is an auxiliary qubit and `B` means it is a data qubit. A SWAP
therefore has one of three *label pairs*: both auxiliary, written AA; one of
each, written AB; or both data, written BB.

A *fault-tolerant SWAP*, abbreviated FT SWAP, is a SWAP realised in a way that
prevents a single fault from spreading into an uncorrectable error. A
*bare SWAP* is one realised without that protection. Which SWAPs must be
fault-tolerant is decided outside this repository, by a simulator, and is not
something this pass computes. The pass only needs to know how *likely* a given
SWAP is to require fault tolerance, so that it can weigh expected cost.

The cost the pass assigns to one candidate SWAP is computed in the `Node`
constructor in `Mapping.cpp` at approximately lines 215 to 230. The relevant
lines today are, as indented source:

    float base = 1.0F;
    if (useTypedCost) { ... base = typedSwapCost(qubitLabels[prog0], qubitLabels[prog1]); }
    const float edgeMultiplier =
        (useEdgeCost && nnnEdges.contains(swap)) ? nnnCostMultiplier : 1.0F;
    pathCost += base * edgeMultiplier;

The helper `typedSwapCost`, at approximately line 267, returns one when both
labels are auxiliary, two when they differ, and three when both are data. The
option `nnnCostMultiplier` defaults to three. The accumulated `pathCost` is
consumed by `Node::g`, at approximately line 259, which returns
`alpha * pathCost` when either opt-in cost heuristic is active and
`alpha * depth` otherwise.

The six costs the current model can produce are therefore one, two and three on
NN edges and three, six and nine on NNN edges, for label pairs AA, AB and BB
respectively.

The measured costs, expressed as multiples of the cost of a bare SWAP between
two auxiliary qubits on an NN edge, are approximately 1.0, 3.2 and 6.5 on NN
edges and 5.2, 11.0 and 19.5 on NNN edges, for the same three label pairs. These
follow from four hardware and decomposition facts. The hardware owner
re-confirmed the third and fourth of them on 2026-09-23, which were the two that
had never been stated in their own words: an NN fault-tolerant SWAP is two bare
NN SWAPs plus one bare NNN SWAP, and an NNN fault-tolerant SWAP is three of
those. Both had previously been inferred from the phrase "2x NN CZ + 1x NNN CZ"
read in SWAP units rather than CZ units, an inference recorded in
`cda-lab-notes/qec-rydberg-ions/2026-09-16_paper_framing.md` and justified there
only by consistency with the simulator's nine-CZ fault-tolerant SWAP. The first
two facts, the CZ fidelities and the three-CZ bare SWAP, still rest on the
hardware group's figures as transcribed into that note and should be
re-confirmed before publication:

First, the native two-qubit entangling gate is CZ. A CZ on an NN edge has
fidelity 0.9975 and a CZ on an NNN edge has fidelity 0.9861, so their
infidelities are 0.0025 and 0.0139 respectively, a ratio of about 5.56.

Second, a bare SWAP is three CZ gates, executed on the edge being swapped
across.

Third, an NN fault-tolerant SWAP is two bare NN SWAPs plus one bare NNN SWAP,
that is six NN CZ gates and three NNN CZ gates.

Fourth, an NNN fault-tolerant SWAP is three NN fault-tolerant SWAPs, that is
eighteen NN CZ gates and nine NNN CZ gates.

Summing infidelities, which is valid because the individual error rates are
small, gives the expected error of a bare SWAP as 1.00 on an NN edge and 5.56 on
an NNN edge, and of a fault-tolerant SWAP as 7.56 on an NN edge and 22.68 on an
NNN edge, all relative to the bare NN SWAP.

The probability that a SWAP requires fault tolerance was measured externally by
replaying two routed Bacon-Shor circuits against simulator verdicts, over one
hundred and sixty SWAPs in total. It is approximately 0.02 for AA, 0.40 for AB
and 0.96 for BB. These three numbers are circuit-family-specific and are
therefore options rather than constants.

Combining, the expected cost of a candidate SWAP is the probability-weighted
average of its bare and fault-tolerant costs on the edge it uses.

## Plan of Work

The work is four small edits in two files plus one new test file section.

In `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`, inside the
`let options` list of `def MappingPass`, add three options after the existing
`nnnCostMultiplier` entry. Add `nnCzFidelity`, command-line name
`nn-cz-fidelity`, of type `float`, default `0.0F`, documented as the measured
fidelity of the native two-qubit entangling gate on a nearest-neighbour coupling
edge, where zero disables the fidelity-weighted cost model. Add `nnnCzFidelity`,
command-line name `nnn-cz-fidelity`, of type `float`, default `0.0F`, documented
the same way for next-nearest-neighbour edges. Add `ftProbabilities`,
command-line name `ft-probabilities`, of type `std::string`, default the empty
string, documented as a comma-separated list of exactly three probabilities
between zero and one giving the likelihood that a SWAP between two auxiliary
qubits, between one auxiliary and one data qubit, and between two data qubits
respectively must be realised fault-tolerantly. Extend the pass's existing
description block to explain the new model in the same prose style used for the
two existing heuristics.

In `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`, add a private member
to the `MappingPass` struct holding the precomputed table, for example
`std::array<float, 6> swapCostTable{};` together with a
`bool useFidelityCost{false};`. Index it as edge kind times three plus label
pair index, where edge kind is zero for NN and one for NNN, and label pair index
is zero for AA, one for AB and two for BB.

In the same file, add a parsing helper beside the existing `parseQubitLabels`
and `parseNnnEdges` helpers, which are at approximately lines 489 and 522. The
new helper must reject a `ftProbabilities` string that does not contain exactly
three comma-separated values, or that contains a value outside the closed
interval from zero to one, emitting a diagnostic through the same mechanism the
existing two helpers use. Call it from `runOnOperation` alongside the existing
two parse calls, which are at approximately lines 411 to 432.

Still in `runOnOperation`, after parsing succeeds, populate the table. Enable
the new model only when both fidelities are strictly greater than zero and
strictly less than one, and `ftProbabilities` parsed successfully, and
`qubitTypeLabels` is non-empty, and `nnnEdges` is non-empty. If any of those is
missing, leave `useFidelityCost` false so behaviour is unchanged. Compute the
bare and fault-tolerant expected errors from the four decomposition facts stated
in the orientation section, normalise all six by the bare NN value so the
numbers stay close to one, and store them.

In the `Node` constructor, replace the cost expression quoted in the orientation
section with a branch that uses the table when `useFidelityCost` is set and
otherwise falls through to the existing multiplicative expression unchanged. The
table's inputs are whether the SWAP's site pair is in `nnnEdges` and the label
pair of the two program qubits, both of which the existing code already
computes.

Finally, add a statistic beside the existing `numSwaps` one in the tablegen
declaration, named `expectedError` with command-line name
`expected-error-milli`, documented as the routed program's total expected SWAP
error in units of one thousandth of a bare nearest-neighbour SWAP. Accumulate it
in the final hot routing pass alongside `numSwaps`, which happens at
approximately line 476 of `Mapping.cpp`. It must be an integer because MLIR
statistics are integers, hence the milli scaling.

## Concrete Steps

Work from the repository root at `/Users/yudong/Documents/projects/mqt.core`.

Configure and build the MLIR unit tests. The repository uses CMake presets:

    cmake --preset release
    cmake --build --preset release --target mqt-core-mlir-unittest-rydberg-ions
    cmake --build --preset release --target mqt-core-mlir-unittest-mapping

Run the existing mapping tests first, before making any edit, and record that
they pass. This is the baseline that the opt-in default must preserve:

    ./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping

Expect a transcript ending in a line reporting all tests passed. Note the number
of tests so you can confirm it does not change.

After making the edits, rebuild both targets and run both binaries. The mapping
test binary must report exactly the same number of passing tests as the
baseline, because every existing test leaves the new options at their defaults.

To observe the new behaviour on the Bacon-Shor circuit, run the Rydberg-ion
binary, which prints a routing analysis dump to standard output:

    ./build/release/mlir/unittests/Dialect/QCO/Transforms/RydbergIons/mqt-core-mlir-unittest-rydberg-ions

## Validation and Acceptance

Two new GoogleTests belong in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`, alongside the
existing tests for the two other cost heuristics.

The first test asserts that the new model is inert by default. Construct any
target with an explicit topology, route a small circuit twice with identical
`MappingPassOptions` except that the second sets `nnCzFidelity` and
`nnnCzFidelity` to zero explicitly, and assert the two routed modules are
structurally identical. Acceptance is that the test passes without any change to
the existing multiplicative code path.

The second test asserts that the model produces the ranking the existing model
cannot express. Build a target containing both an NN edge and an NNN edge such
that the router must choose between swapping two auxiliary-labelled qubits
across the NNN edge and swapping two data-labelled qubits across the NN edge.
Under the existing multiplicative model these cost three and three, a tie. Under
the new model they cost approximately 5.2 and 6.5, so the auxiliary pair on the
NNN edge must win. Assert on the operands of the emitted `qco.swap` operation,
following the assertion style the existing `qubitTypeLabels` test already uses.
Acceptance is that the routed circuit swaps the auxiliary pair, and that the
same test with the fidelity options omitted does not.

Beyond the unit tests, the behavioural acceptance for the overall purpose is
that routing the Bacon-Shor circuit with the new model reports a lower
`expected-error-milli` statistic than routing it with the multiplicative model,
at equal or comparable `num-inserted-swaps`. Record both figures in
`Outcomes & Retrospective`. If the swap count rises while expected error falls,
that is a success, not a regression, and the retrospective must say so
explicitly, because swap count is the metric this pass historically optimised
and a reader will otherwise misread the result.

## Idempotence and Recovery

Every step is repeatable. The build steps are incremental and safe to rerun. No
step mutates any file outside the repository or any checked-in test fixture.

The risky part is the edit to the `Node` constructor, because it sits on the A*
search's hot path and is shared by every target, not only the Rydberg-ion one.
If the mapping test binary reports any failure after the edit, the recovery path
is to confirm that `useFidelityCost` is false in that test's configuration and
that the fall-through branch is byte-for-byte the original expression. Reverting
is a matter of restoring that single expression; nothing else in the change can
affect existing behaviour, because every other addition is gated behind options
that default to disabled.

## Artifacts and Notes

The measured probabilities came from replaying the routed circuits in
`/Users/yudong/Documents/projects/qec-rydberg-ions/scripts/test_cases/4.in` and
`5.in` against the per-SWAP fault-tolerance verdicts recorded in the
corresponding checkpoint files under
`/Users/yudong/Documents/projects/qec-rydberg-ions/scripts/output/`. Pooled over
one hundred and sixty SWAPs the counts were one of forty-nine for AA,
thirty-five of eighty-eight for AB, and twenty-two of twenty-three for BB.

A caveat the implementer must carry forward: those verdicts come from a greedy,
single-pass, order-dependent search, so they are an upper bound on how many
SWAPs genuinely require fault tolerance rather than an exact count. The two
source circuits were also walked in different directions. The probabilities are
therefore good enough to calibrate a routing heuristic but must not be presented
as ground truth.

## Interfaces and Dependencies

No new libraries or services. The change stays inside `MLIRQCOTransforms` and
its existing tablegen-generated options infrastructure.

At the end of this milestone the following must exist. In
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`, the `MappingPass` options
list must contain `nnCzFidelity`, `nnnCzFidelity` and `ftProbabilities` with the
defaults stated above, and the statistics list must contain `expectedError`. The
generated `MappingPassOptions` struct, which callers construct directly, must
therefore gain three correspondingly named fields; the existing caller in
`mlir/lib/Compiler/TargetCompilation.cpp`, which constructs a default
`MappingPassOptions{}`, must continue to compile unchanged.
