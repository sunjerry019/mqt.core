# Let the user supply an initial qubit layout to the mapping pass, as a seed or as a fixed choice

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

## Purpose / Big Picture

Before the compiler can route a quantum circuit onto hardware it must decide
which hardware position each of the program's qubits starts in. That decision is
called the *initial layout*, and it matters a great deal: a good starting
arrangement means fewer qubits have to be moved around later, and moving qubits
is the expensive part.

Today the pass always invents that arrangement itself, by trying several random
arrangements and keeping whichever looks best. There is no way to tell it about
an arrangement you already know is good. This matters for two concrete reasons.
Someone working on this hardware has hand-designed layouts that took real effort
to find and that the automatic search does not currently match. And anyone
trying to measure whether a change to the router helps has no way to hold the
layout fixed, so every measurement mixes up two different effects: how good the
layout was, and how good the routing from it was.

After this change, a user can pass an initial layout to the pass in either of
two ways. In the default *seed* mode the supplied layout joins the pool of
candidate layouts, is refined and scored exactly like the randomly generated
ones, and wins only if it is genuinely the best of them; a stale or mistaken
layout therefore degrades gracefully into "ignored" rather than into a worse
circuit. In *pin* mode the supplied layout is used exactly as given and no
search happens at all, which is what you want when replaying a known compilation
or when holding the layout constant to measure something else.

You can see it working by routing a circuit with a deliberately chosen layout in
pin mode and confirming from the pass's own report that every program qubit
started on the site you named; and by routing with an obviously terrible layout
in seed mode and confirming that the result is no worse than routing without it.

## Progress

- [ ] Read the orientation section and confirm the named code matches the
      current working tree, in particular the shape of `generateLayout` and the
      `Layout::fromMapping` contract.
- [ ] Add the two pass options to the tablegen declaration.
- [ ] Add the parser and its diagnostics.
- [ ] Wire the parsed layout into `generateLayout` for both seed and pin mode.
- [ ] Add the three GoogleTests described under Validation and Acceptance.
- [ ] Build, run the mapping tests, and record results in
      `Outcomes & Retrospective`.

## Surprises & Discoveries

- Observation: everything needed to construct a layout from an explicit mapping
  already exists and is already used by this pass. Evidence:
  `Layout::fromMapping(ArrayRef<size_t>)` is declared at
  `mlir/include/mlir/Dialect/QCO/Utils/Layout.h` line 37 and is already called
  inside `generateLayout` at `Mapping.cpp` line 1031 for the case where the
  target has no explicit topology. The new work is therefore parsing and
  plumbing, not algorithm.

- Observation: pinning the layout does not make the pass reproducible. Evidence:
  `mlir/include/mlir/Dialect/QCO/Utils/Drivers.h` line 37 declares
  `using ReadyMap = llvm::SmallDenseMap<Operation*, SmallVector<size_t>, 8>;`,
  which is keyed on heap addresses and iterated during routing at `Mapping.cpp`
  line 1354, so the iteration order varies between process launches. Five runs
  of the Rydberg-ion test with identical inputs produced 83, 85, 86, 86 and 93
  inserted SWAP operations. Pinning removes the variation that comes from the
  random layout search, and nothing else. Anyone expecting this option to
  reproduce a specific compilation byte for byte will be disappointed, and the
  option's documentation must say so.

## Decision Log

- Decision: make seeding the default behaviour when a layout is supplied, and
  put pinning behind a separate boolean. Rationale: the stated purpose has two
  distinct users. One knows a good layout and wants the compiler to benefit from
  it, and is best served by having it compete, because the supplied layout may
  be stale or may have come from a different circuit; the other wants exactly
  that layout and nothing else, for replay or for controlled measurement.
  Seeding is the safer default because its failure mode is "no effect" whereas
  pinning's failure mode is "silently worse output". Date/Author: 2026-09-18,
  this plan.

- Decision: in seed mode, refine the supplied layout with the same
  forward-and-backward refinement applied to the random candidates, rather than
  entering it unrefined. Rationale: scoring candidates that have had different
  amounts of work done to them is not a fair comparison, and the refinement can
  only improve a layout that is already good. The consequence, which the
  option's documentation must state, is that the layout actually used in seed
  mode is usually not the one supplied. Date/Author: 2026-09-18, this plan.

- Decision: express the layout as a comma-separated list of hardware site
  indices in program-qubit order, matching the `mapping[prog] = hw` convention
  that `Layout::fromMapping` already documents, rather than inventing a second
  convention. Rationale: there are two possible readings of a permutation and
  choosing the one the existing API already uses removes a whole class of silent
  off-by-permutation bugs. Date/Author: 2026-09-18, this plan.

- Decision: validate the string in the pass and emit an MLIR diagnostic, rather
  than letting `Layout::fromMapping` reject it. Rationale: `fromMapping` calls
  `llvm::reportFatalUsageError` on a non-permutation, which terminates the
  process; a user typo in a command-line option must produce an error message
  and a non-zero exit, not a crash. Date/Author: 2026-09-18, this plan.

## Outcomes & Retrospective

Not yet started. On completion, record the SWAP count obtained from a pinned
hand-designed layout against the SWAP count the automatic search obtains on the
same circuit, and whether seeding with that layout improved on the automatic
search.

## Context and Orientation

Everything below is a fact about the repository as it stands. Verify it.

Terms, in plain language. A *site*, also called a hardware qubit, is a physical
qubit position on the machine, numbered from zero. A *program qubit* is a qubit
of the user's circuit. A *layout* is the assignment of program qubits to sites;
it changes as the circuit runs, because SWAP operations exchange the contents of
two sites. The *initial layout* is the assignment before anything has run.
*Routing* is the insertion of SWAP operations so that every multi-qubit gate
acts on sites the hardware directly connects.

The pass is declared in `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` as
`def MappingPass`, command-line name `place-and-route`, and implemented as the
single `MappingPass` struct in
`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`. Its factory is
`createMappingPass(const CompilerTarget& target, MappingPassOptions options)`,
declared in `mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h`. The
options struct `MappingPassOptions` is generated by tablegen from the options
list in the `.td` file, so adding an option there adds a field to that struct.

The `Layout` class is `mlir/include/mlir/Dialect/QCO/Utils/Layout.h`,
implemented in `mlir/lib/Dialect/QCO/Utils/Layout.cpp`. Two of its static
constructors matter here. `Layout::random(size_t nqubits, size_t seed)` builds
the identity mapping and shuffles it.
`Layout::fromMapping(ArrayRef<size_t> mapping)` builds a layout from an explicit
mapping where, quoting the header's own comment, `mapping[prog] = hw`.
`fromMapping` checks that its argument is a permutation of zero through
`mapping.size() - 1` and calls `llvm::reportFatalUsageError` with the message
`mapping must be a permutation` if it is not. That call terminates the process,
which is why this plan validates first.

The function to change is `generateLayout`, at approximately lines 1029 to 1079
of `Mapping.cpp`. Its current structure, as indented source, with commentary
removed:

    if (!target->hasExplicitTopology()) {
      return Layout::fromMapping(llvm::to_vector(llvm::seq(target->numQubits())));
    }
    std::mt19937_64 rng{seed};
    struct Trial { RoutingBundle bundle; Statistics stats{}; bool success{false}; };
    SmallVector<Trial, 0> trials;
    trials.reserve(ntrials);
    for (size_t i = 0; i < ntrials; ++i) {
      trials.emplace_back(RoutingBundle{.wires = wires, .infos = infos,
          .layout = Layout::random(target->numQubits(), rng())});
    }
    parallelForEach(&getContext(), trials, [&, this](Trial& t) {
      for (size_t i = 0; i < niterations; ++i) {
        if (route<WireDirection::Forward>(t.bundle, t.stats).failed()) { return; }
        t.stats.nswaps = 0;
        if (route<WireDirection::Backward>(t.bundle, t.stats).failed()) { return; }
      }
      t.success = true;
    });
    Trial* best = nullptr;
    for (Trial& t : trials) {
      if (t.success && (best == nullptr || best->stats.nswaps > t.stats.nswaps)) {
        best = &t;
      }
    }
    if (best == nullptr) { return failure(); }
    return best->bundle.layout;

`generateLayout` is called from `runOnOperation` at approximately line 453,
immediately after the pass has checked that the program does not need more
qubits than the target has. The returned layout is then handed to `place` and to
the single real routing pass.

Program qubits are numbered in the order that `discoverComputation`, at
approximately line 740 of the same file, walks the entry function and encounters
qubit allocation operations, that is `AllocOp` and `qtensor::AllocOp` in the
function body. That is ordinary MLIR walk order, so it is the textual order of
the allocations in the input. This is the numbering the supplied layout string
must use, and the option's documentation must say so explicitly, because a
reader will otherwise assume it is the order the qubits happen to be named in.

Note that the layout covers *sites*, not program qubits: `Layout::random` is
called with `target->numQubits()`. A program using fewer qubits than the target
has still gets a full-width layout, and the supplied string must therefore have
one entry per site, not one per program qubit. The pass already rejects programs
needing more qubits than the target has, at approximately line 444.

Two existing options establish the pattern to follow, both opt-in and both
defaulting to off: `qubitTypeLabels`, a string with one character per program
qubit, and `nnnEdges`, a comma-separated list of site pairs such as
`2-4,5-7,8-10`. Their parsers are `parseQubitLabels` at approximately line 489
and `parseNnnEdges` at approximately line 522, both returning `FailureOr<...>`,
and both are called from `runOnOperation` at approximately lines 411 to 432
where a failure emits a diagnostic through `func.emitError()` and calls
`signalPassFailure()`.

## Plan of Work

In `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`, inside the
`let options` list of `def MappingPass`, add two options.

Add `initialLayout`, command-line name `initial-layout`, of type `std::string`,
default the empty string. Document it as a comma-separated list of hardware site
indices giving the site each program qubit starts on, with one entry per site of
the target, in the order that qubit allocations appear in the entry function;
and state that the empty default means the pass chooses the initial layout
itself. State in the same documentation that the list must be a permutation of
zero through one less than the number of sites.

Add `pinInitialLayout`, command-line name `pin-initial-layout`, of type `bool`,
default `false`. Document it as meaning that the layout given by `initialLayout`
is used exactly as supplied, with no search and no refinement; and that when it
is false, which is the default, the supplied layout instead becomes one
candidate among `ntrials` candidates and is refined and scored like the others,
so the layout finally used is usually not the one supplied. Document that
`pinInitialLayout` has no effect when `initialLayout` is empty.

Extend the pass's description block in the same file with a prose paragraph, in
the style of the paragraphs that describe the two existing heuristics,
explaining the two modes and stating plainly that pinning the layout does not
make the pass reproducible run to run.

In `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`, add a parsing helper
beside `parseQubitLabels` and `parseNnnEdges`, named `parseInitialLayout`,
taking the option string and the target's site count and returning
`FailureOr<SmallVector<size_t>>`. It returns an empty vector for an empty
string. Otherwise it splits on commas, parses each element as an unsigned
integer, and fails if any element is not a valid integer, if the number of
elements differs from the site count, if any element is greater than or equal to
the site count, or if any value appears twice. Failing on all four conditions
before constructing a `Layout` is the whole point of the helper, because
`Layout::fromMapping` would terminate the process instead.

Call it from `runOnOperation` alongside the existing two parse calls, storing
the result in a new private member of the `MappingPass` struct, for example
`SmallVector<size_t> initialLayoutMapping;`. On failure emit a diagnostic
through `func.emitError()` naming the offending option string and the specific
reason, following the wording style of the two existing failure messages, then
call `signalPassFailure()` and return.

In `generateLayout`, make two changes.

First, immediately after the existing `hasExplicitTopology` early return, add a
second early return: if `initialLayoutMapping` is non-empty and
`pinInitialLayout` is set, return `Layout::fromMapping(initialLayoutMapping)`
directly. No trials are built, no refinement runs, and the supplied layout is
what the rest of the pass uses.

Second, in the loop that builds the trials, if `initialLayoutMapping` is
non-empty, give the first trial the supplied layout instead of a random one, and
leave trials one through `ntrials - 1` random as they are. Draw the random seed
for every random trial exactly as today, that is by calling `rng()` once per
random trial; do not call `rng()` for the seeded trial, and note in a comment
that this changes which random layouts the remaining trials receive compared
with a run that supplies no layout, which is expected and harmless. If `ntrials`
is one, the supplied layout is the only candidate and is still refined, which
differs from pin mode and is the intended distinction.

Everything downstream is unchanged. The refinement loop and the selection loop
treat the seeded trial exactly like the others.

If the ExecPlan `.agent/plans/cost-weighted-trial-selection.md` has already been
implemented, the selection loop compares weighted cost rather than raw SWAP
count when a cost heuristic is active, and the seeded trial competes on that
basis instead. No change is needed here either way; the two plans compose. If it
has not been implemented, note in `Surprises & Discoveries` that a supplied
layout is being judged by raw SWAP count even when the search was minimising
something else, which is a known reason a good supplied layout might lose.

## Concrete Steps

Work from the repository root at `/Users/yudong/Documents/projects/mqt.core`.

Establish a baseline before editing:

    cmake --preset release
    cmake --build --preset release --target mqt-core-mlir-unittest-mapping
    ./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping

Expect a transcript ending in a line reporting all tests passed; record the
count. After the edits, rebuild and rerun; the count must be identical, because
every existing test leaves `initialLayout` empty.

To exercise the option end to end, build and run the Rydberg-ion routing tool if
the ExecPlan `.agent/plans/rydberg-ion-routing-tool.md` has been completed:

    cmake --build --preset release --target mqt-rydberg-route
    ./build/release/bin/mqt-rydberg-route --pin-initial-layout \
        --initial-layout=8,0,11,2,9,5,6,4,3,7,1,10

That particular permutation is the initial layout recorded in the checked-in
report at
`/Users/yudong/Documents/projects/qec-rydberg-ions/scripts/test_cases/1.in`,
whose qubit-to-site section reads `data0: 8 -> 11`, `data1: 0 -> 0`,
`data2: 11 -> 10`, `data3: 2 -> 1`, `data4: 9 -> 8`, `data5: 5 -> 4`,
`data6: 6 -> 3`, `data7: 4 -> 2`, `data8: 3 -> 6`, `ancilla0: 7 -> 9`,
`ancilla1: 1 -> 5`, `ancilla2: 10 -> 7`. Expect the tool's own
`program qubit -> physical site` section to report exactly those initial sites,
and expect the final sites and the SWAP count to differ from that file, because
the routing itself is not reproducible.

If that tool does not exist yet, exercise the option by editing the
`MappingPassOptions` literal in
`mlir/unittests/Dialect/QCO/Transforms/RydbergIons/test_rydberg_ions.cpp` and
rerunning that test binary, which prints the same section.

## Validation and Acceptance

Add three GoogleTests to
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`, beside the
existing tests for the other pass options.

The first proves pin mode is obeyed exactly. Route a small circuit on a target
with an explicit topology, with `pinInitialLayout` true and `initialLayout` set
to a permutation chosen to be different from the identity and different from
what the search would plausibly pick. Assert that each program qubit's starting
site is the one named. Recover the starting sites the way the Rydberg-ion test
does, by walking backwards from each qubit's last value across SWAP operations,
crossing to the *other* input at each SWAP; the helper `traceToInitialSite`, at
approximately line 499 of
`mlir/unittests/Dialect/QCO/Transforms/RydbergIons/test_rydberg_ions.cpp`, shows
the exact traversal and its comment explains why it crosses. Assert also that
the routed circuit is still correct, meaning every multi-qubit operation acts on
connected sites, using the same executability check that file's `isExecutable`
performs.

The second proves seed mode is safe. Route a circuit twice with `ntrials`
greater than one and an identical seed: once with no supplied layout, and once
with `initialLayout` set to a deliberately poor permutation, for example one
that places qubits that interact frequently at opposite ends of the topology.
Assert that the second run's SWAP count is not greater than the first run's.
Because routing varies between process launches this assertion must be made
within a single process, which the test naturally satisfies, and it must compare
the two runs made in that process rather than any hardcoded number. If this
assertion proves flaky in practice, the correct response is to raise `ntrials`,
not to weaken the assertion, and the reason must be recorded in
`Surprises & Discoveries`.

The third proves bad input is diagnosed rather than fatal. Run the pass with
`initialLayout` set, in turn, to a string with too few entries, to one
containing a repeated site index, and to one containing a non-numeric element.
Assert that the pass signals failure and emits a diagnostic in each case, using
whatever diagnostic-capturing idiom the surrounding tests already use. This test
exists specifically because the underlying `Layout::fromMapping` would otherwise
terminate the test binary, so its failure mode if the validation is missing is a
crash rather than a failed assertion.

The overall acceptance is that the full mapping test binary reports the same
number of passing tests as the recorded baseline plus the three added here, and
that the pin-mode invocation under `Concrete Steps` reports the initial sites it
was given.

## Idempotence and Recovery

All steps are repeatable and nothing outside the build directory is modified.

The failure mode to watch for is a layout that parses but means something other
than intended, because the program-qubit numbering is not what the user assumed.
The symptom is a pinned run whose reported initial sites are a permutation of
the ones requested rather than the ones requested. The first test above is
written to catch exactly that, and the option's documentation must state the
numbering rule.

If the mapping tests regress, confirm that the new code paths are reached only
when `initialLayout` is non-empty, which no pre-existing test sets.

## Artifacts and Notes

The reason this option is worth having beyond user convenience: it separates two
things that are currently measured together. Given a hand-designed layout known
to be good, pinning it and routing from it answers "how much of the quality of a
hand-made compilation comes from the placement, and how much from the routing?"
That question cannot be asked at all today, and its answer determines whether
further effort belongs in the layout search or in the router.

For the evaluation campaign described in
`/Users/yudong/Documents/projects/qec-rydberg-ions/.agent/plans/evaluation-protocol.md`,
pin mode is the instrument that removes layout variance from a measurement, and
seed mode is a configuration under test in its own right.

## Interfaces and Dependencies

No new libraries. The change is confined to `MLIRQCOTransforms` and its tablegen
declaration.

At the end of this milestone the following must exist. `def MappingPass` in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` must declare options
`initialLayout`, command-line name `initial-layout`, defaulting to the empty
string, and `pinInitialLayout`, command-line name `pin-initial-layout`,
defaulting to false. The generated `MappingPassOptions` struct gains two
correspondingly named fields, and the existing caller in
`mlir/lib/Compiler/TargetCompilation.cpp`, which constructs a default
`MappingPassOptions{}`, must continue to compile unchanged. `Mapping.cpp` must
contain a `parseInitialLayout` helper that rejects every malformed input with an
MLIR diagnostic rather than a fatal error.
