# Turn the Rydberg-ion routing test into a scriptable command-line tool

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

## Purpose / Big Picture

Today the only way to route the Bacon-Shor quantum error correction circuit onto
the twelve-site Rydberg-ion trapped-ion architecture is to run a GoogleTest
binary that prints a routing report to standard output. Changing any routing
parameter means editing constants in C++ source and rebuilding. That makes it
impossible to sweep parameters, to route the same circuit many times under
different random seeds, or to drive routing from a script.

After this change, a user can run a single executable, pass every routing
parameter as a command-line flag, and capture a machine-readable routing report
from standard output. Concretely, they will be able to run the routing a hundred
times with a hundred different seeds from a shell loop and collect a hundred
reports, without recompiling anything.

You can see it working by running the new tool twice with two different seeds
and observing that the reported number of inserted SWAP operations differs, and
by running it with the `--emit=json` flag and piping the result into any JSON
parser.

One deliberate piece of forward-looking design is included, because it is nearly
free now and expensive to retrofit. Two things that are hardcoded in the
existing test will eventually have to vary: which circuit is routed, and what
the hardware looks like. Other quantum error correction codes are planned for
this pipeline, and they do not fit the current machine description: the Steane
code circuit needs fourteen qubits and the surface code circuit needs seventeen,
against the twelve sites the current target declares, so each will need a
different set of sites and connections. This plan therefore selects the circuit
by name from a small registry and builds the hardware description from a
description that can be supplied on the command line, with today's Bacon-Shor
circuit and today's twelve-site machine as the defaults. Adding a second circuit
later becomes an addition to a registry rather than a restructuring of the tool.
Nothing else about the current behaviour changes, and a user who passes no flags
gets exactly what the test does today.

## Progress

- [ ] Read the orientation section and confirm the named functions exist at the
      stated locations in the current working tree.
- [ ] Create the shared static library target and move the circuit, target, and
      reporting code into it.
- [ ] Repoint the existing GoogleTest at the shared library and confirm it still
      passes unchanged.
- [ ] Create the new tool directory, source file, and CMake target.
- [ ] Implement the command-line interface and the two output formats.
- [ ] Implement the named-circuit registry and the command-line hardware
      description, both defaulting to today's behaviour.
- [ ] Validate per the acceptance section and record observed output in
      `Outcomes & Retrospective`.

## Surprises & Discoveries

- Observation: the routing pass does not produce identical output across
  repeated runs of the same binary with identical inputs and an identical seed.
  Evidence: five consecutive runs of the existing test binary produced 83, 85,
  86, 86 and 93 inserted SWAP operations. The cause is that
  `mlir/include/mlir/Dialect/QCO/Utils/Drivers.h` declares
  `using ReadyMap = llvm::SmallDenseMap<Operation*, SmallVector<size_t>, 8>;`, a
  hash map keyed on raw heap pointer values, and
  `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` iterates it directly to
  decide which of several simultaneously-ready gates to process next. Heap
  addresses vary between process launches, so ties break differently each time.
  This plan does not fix that; it is recorded here because a reader will
  otherwise assume the tool is broken when two identical invocations disagree.

## Decision Log

- Decision: extract the circuit construction, target definition, and reporting
  code into a shared static library consumed by both the existing GoogleTest and
  the new tool, rather than copying it into the tool or deleting the test.
  Rationale: the reporting code is the definition of the output format that a
  downstream Python project already parses, so having two copies would let the
  formats silently diverge; keeping the test preserves the existing correctness
  assertions, which the tool does not perform. Date/Author: 2026-09-18, this
  plan.

- Decision: build the tool with `add_mlir_tool` under `mlir/tools/`, alongside
  the existing `mqt-cc` driver, rather than as another unit-test binary.
  Rationale: `mlir/tools/` is where this repository puts executables meant to be
  invoked by users, and `add_mlir_tool` supplies the LLVM command-line plumbing
  and install rules that a user-facing binary needs. Date/Author: 2026-09-18,
  this plan.

- Decision: emit the existing human-readable report by default and add a JSON
  format behind a flag, rather than switching the default to JSON. Rationale: a
  downstream Python project already parses the existing format from checked-in
  files, so changing the default would break it; JSON is added because the
  existing format requires the consumer to strip surrounding noise, which is a
  known source of parse failures. Date/Author: 2026-09-18, this plan.

- Decision: select the circuit by name from a registry rather than hardcoding
  the only circuit that exists. Rationale: the Steane and surface code circuits
  are planned for this pipeline and will each be a separate builder function; a
  registry makes adding one a local change, whereas a tool that constructs one
  circuit unconditionally would have to be restructured. The registry has
  exactly one entry when this plan is complete, which is the point: it costs
  almost nothing now. Date/Author: 2026-09-18, this plan.

- Decision: allow the hardware description to be supplied on the command line,
  defaulting to the twelve-site machine the test uses today. Rationale: the
  planned circuits need fourteen and seventeen qubits respectively, so the set
  of sites and connections must change per circuit; a tool whose machine
  description is a compiled-in constant would need a code change and a rebuild
  for each, which is exactly the workflow this tool exists to remove.
  Date/Author: 2026-09-18, this plan.

- Decision: print the chosen initial layout in the same comma-separated form
  that the mapping pass's `initial-layout` option accepts. Rationale: this makes
  the tool's output feedable back into the tool, so a layout found by one run
  can be replayed or used as a starting point by a later run, which is the
  workflow the ExecPlan `.agent/plans/initial-layout-option.md` exists to
  support. Without it the layout is reported only in a prose section that no
  option can consume. Date/Author: 2026-09-18, this plan.

## Outcomes & Retrospective

Not yet started. On completion, record here the exact command lines used, the
SWAP counts observed across several seeds, and confirmation that the existing
GoogleTest still reports the same number of passing tests as before the change.

## Context and Orientation

Everything below is a fact about the repository as it stands. Verify each claim
before relying on it.

Some terms, in plain language. A *target* is a description of hardware: how many
qubit sites it has, which pairs are directly connected, and which operations it
can perform natively. It is the `CompilerTarget` class in
`mlir/include/mlir/Compiler/Target.h`. *Routing* is the process of inserting
SWAP operations into a quantum circuit so that every multi-qubit gate acts on
sites that are directly connected. The *mapping pass* is the compiler pass that
does this; it is declared in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` as `def MappingPass` with
the command-line name `place-and-route`, implemented in
`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`, and constructed through
`createMappingPass(const CompilerTarget& target, MappingPassOptions options)`
declared in `mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h`.

All the code this plan moves currently lives in one file,
`mlir/unittests/Dialect/QCO/Transforms/RydbergIons/test_rydberg_ions.cpp`, which
is about 672 lines. Its CMake target is declared in the sibling `CMakeLists.txt`
as `mqt-core-mlir-unittest-rydberg-ions` and links against `GTest::gtest_main`,
`MLIRParser`, `MQTCompilerTarget`, `MLIRQCOProgramBuilder`, `MLIRQTensorUtils`,
`MLIRQCOTransforms`, `MLIRQCOToQC`, `MLIRQCOpenQASMTranslation` and
`MLIRSupportMQT`.

The pieces to move, with their approximate current line numbers:

`runPass` at line 99 builds a pass manager, adds the mapping pass, runs it, then
applies the canonicalization patterns of `SinkOp`. `isExecutable` at line 131
and its overload at line 189 verify that every multi-qubit operation ended up on
mutually connected sites. `getRydbergIonTarget` at line 209 returns the
twelve-site target, whose coupling edges are the pairs 0-1, 1-2, 2-3, 2-4, 3-4,
4-5, 5-6, 5-7, 6-7, 7-8, 8-9, 8-10, 9-10 and 10-11, and which declares the
three-qubit operations named `ccx` and `ccz` as native. The stabilizer tables
`kSx` at line 244 and `kSz` at line 252 drive four circuit-building functions:
`buildXSyndromeCircuit` at 274, `buildXCorrectionCircuit` at 300,
`buildZSyndromeCircuit` at 317 and `buildZCorrectionCircuit` at 338, combined by
`buildBaconShorCircuit` at 362. `buildAndFinalizeBaconShorProgram` at 424 adds
measurements and a barrier. `struct QubitTrace` at 394 records, for each named
program qubit, the first and last operation that uses it. `traceToInitialSite`
at 499 walks backwards across SWAP operations to recover which site a program
qubit started on. `routedProgramToOpenQASM3` at 519 clones the module, converts
it, and translates it to OpenQASM 3 text. `dumpRoutedProgram` at 541 prints the
report.

The report format that `dumpRoutedProgram` produces, and that this tool must
keep producing, consists of a banner line of equals signs, the line
`Bacon-Shor routing analysis dump`, another banner, a line beginning `options:`
listing every routing parameter, a section headed
`--- routed program (gates + inserted qco.swap ops) ---` containing the routed
module printed as MLIR, a section headed
`--- program qubit -> physical site (initial -> final) ---` containing one line
per named program qubit, and a section headed
`--- routed program as OpenQASM3 ...  ---` containing OpenQASM 3 source.

A downstream Python project at
`/Users/yudong/Documents/projects/qec-rydberg-ions` parses this report in
`src/mapper_output.py`. Its parser locates the qubit-to-site section and the
OpenQASM 3 section by their headings and ignores everything before the former,
so the MLIR section is harmless. It does however treat everything after the
OpenQASM 3 heading as OpenQASM 3 and raises an error on any line it cannot
parse. That is why the checked-in copies of these reports under
`scripts/test_cases/` have had the trailing banner and the GoogleTest output
manually removed. The new tool must therefore not print anything after the
OpenQASM 3 section.

The routing parameters are the fields of `MappingPassOptions`, generated from
the options list of `def MappingPass` in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`. They are `nlookahead`, a
count of how many upcoming circuit layers the search considers, default one;
`alpha` and `lambda`, two floating-point weights in the search's cost function,
defaults one and one half; `niterations`, how many refinement passes to run,
default one; `ntrials`, how many random starting assignments of program qubits
to sites to try, default four; `seed`, the random seed, default forty-two;
`qubitTypeLabels`, a string with one character per program qubit where `A` marks
an auxiliary qubit and `B` a data qubit, default empty meaning the feature is
off; `nnnEdges`, a comma-separated list of site pairs such as `2-4,5-7,8-10`
marking coupling edges that are physically more expensive, default empty meaning
the feature is off; and `nnnCostMultiplier`, the penalty applied to those edges,
default three.

The existing test invokes the pass with `nlookahead` five, `niterations` one,
`ntrials` one, `seed` forty-two, `qubitTypeLabels` set to nine `B` characters
followed by three `A` characters, and `nnnEdges` set to `2-4,5-7,8-10`.

The existing user-facing tool in this repository is
`mlir/tools/mqt-cc/mqt-cc.cpp`, declared in `mlir/tools/mqt-cc/CMakeLists.txt`
via `add_mlir_tool`. Its parent `mlir/tools/CMakeLists.txt` currently contains a
single `add_subdirectory(mqt-cc)` line. `mqt-cc.cpp` is the reference for how
this repository declares command-line options: it uses `llvm::cl::opt`
declarations at namespace scope and calls `llvm::cl::ParseCommandLineOptions`.

## Plan of Work

First, create the shared library. Add a new directory
`mlir/lib/Dialect/QCO/Testing/RydbergIons/` containing `BaconShor.cpp` and a
matching header at
`mlir/include/mlir/Dialect/QCO/Testing/RydbergIons/BaconShor.h`. Move into it,
unchanged in behaviour, the functions `getRydbergIonTarget`,
`buildBaconShorCircuit` together with its four helper builders and the two
stabilizer tables, `buildAndFinalizeBaconShorProgram`, `struct QubitTrace`,
`traceToInitialSite`, `routedProgramToOpenQASM3`, `dumpRoutedProgram`, and
`runPass`. Declare in the header only what both consumers need: the target
getter, the program builder, the pass runner, and the report printer. Keep
`isExecutable` in the test file, because only the test asserts on it.

Give the new library a CMake target named `MQTRydbergBaconShor`, declared with
`add_mlir_library`, linking the same libraries the existing test links minus
`GTest::gtest_main`. Follow the copyright header format used by every other
`CMakeLists.txt` in this tree.

Second, repoint the test. Edit
`mlir/unittests/Dialect/QCO/Transforms/RydbergIons/test_rydberg_ions.cpp` to
include the new header and delete the moved definitions, and edit its
`CMakeLists.txt` to link `MQTRydbergBaconShor`. The test's two `TEST_F` bodies
and all its assertions must remain exactly as they are.

Third, create the tool. Add `mlir/tools/mqt-rydberg-route/` containing
`mqt-rydberg-route.cpp` and a `CMakeLists.txt` modelled on the `mqt-cc` one,
declaring `add_mlir_tool(mqt-rydberg-route mqt-rydberg-route.cpp)` and linking
`MQTRydbergBaconShor` plus the libraries it needs. Add
`add_subdirectory(mqt-rydberg-route)` to `mlir/tools/CMakeLists.txt`.

In `mqt-rydberg-route.cpp`, declare one `llvm::cl::opt` for each of the nine
routing parameters, named on the command line exactly as the pass option is
named in the tablegen file so that a reader can move between the two without a
translation table, that is `--nlookahead`, `--alpha`, `--lambda`,
`--niterations`, `--ntrials`, `--seed`, `--qubit-type-labels`, `--nnn-edges` and
`--nnn-cost-multiplier`. Give each the same default the tablegen file gives it,
except set the defaults of `--nlookahead` to five, `--qubit-type-labels` to
`BBBBBBBBBAAA` and `--nnn-edges` to `2-4,5-7,8-10`, so that running the tool
with no flags reproduces what the existing test does. Add `--emit`, accepting
`in` or `json`, defaulting to `in`. Add `-o` for an output file, defaulting to
standard output.

Declare flags for any option the mapping pass has gained since this plan was
written, using the same naming rule. In particular, if
`.agent/plans/initial-layout-option.md` has landed, declare `--initial-layout`
and `--pin-initial-layout`; if `.agent/plans/fidelity-weighted-swap-cost.md` has
landed, declare `--nn-cz-fidelity`, `--nnn-cz-fidelity` and
`--ft-probabilities`; and if `.agent/plans/gate-adjacent-swap-bias.md` has
landed, declare `--gate-adjacent-swap-discount`. Each must default to the same
value the tablegen file gives it, so that the no-flags invocation is unaffected.
A simple way to keep this honest is to assemble `MappingPassOptions` field by
field from the flags and nowhere else, so that a forgotten flag shows up as a
compile error when the struct gains a field rather than as a silently ignored
option.

Now make the circuit selectable. Add `--circuit`, a string flag defaulting to
`bacon-shor`. In the shared library, expose a registry mapping a name to a small
record holding the function that builds that circuit into a module, the default
qubit type label string for it, and a human-readable description. Populate it
with the single entry `bacon-shor` whose builder is the existing
`buildAndFinalizeBaconShorProgram` and whose default label string is
`BBBBBBBBBAAA`. If `--qubit-type-labels` is not given explicitly, take it from
the registry entry, so that adding a circuit later does not require the caller
to know its label string. If `--circuit` names something not in the registry,
print the available names to standard error and exit non-zero.

Now make the hardware description selectable. Add `--num-sites`, an unsigned
flag, and `--coupling-map`, a string flag taking a comma-separated list of site
pairs in the same `a-b` form that `--nnn-edges` already uses, and
`--native-gates`, a comma-separated list of operation names to declare as
natively executable. Default all three to empty, meaning "use the built-in
twelve-site Rydberg-ion machine", which is what `getRydbergIonTarget` returns
today. When any of them is supplied, all of `--num-sites` and `--coupling-map`
must be supplied together, and the tool builds a `CompilerTarget` from them
instead; `--native-gates` defaults to `ccx,ccz` because those are the operations
the built-in target declares native. Refactor `getRydbergIonTarget` in the
shared library into a general constructor taking the site count, the edge list
and the native operation names, plus a zero-argument wrapper that supplies
today's values, so that the existing test's call site does not change.

Report the initial layout in machine-readable form. In the `in` format, add one
line immediately after the existing `options:` line, beginning `initial-layout:`
and followed by the comma-separated list of site indices in program-qubit order,
which is exactly the form the mapping pass's `initial-layout` option accepts.
Place it before the existing section headings so that the downstream parser in
`/Users/yudong/Documents/projects/qec-rydberg-ions/src/mapper_output.py`, which
ignores everything before the qubit-to-site heading, is unaffected. In the
`json` format, add a string key `initial_layout` carrying the same text. The
values come from `traceToInitialSite`, which the shared library already
provides.

The body registers the dialects the test registers, builds the program, runs the
pass with the options assembled from the flags, and then either calls the shared
`dumpRoutedProgram` for the `in` format or emits JSON. Because the report must
not be followed by anything, the `in` path must omit the trailing banner line
that `dumpRoutedProgram` currently prints; add a boolean parameter to
`dumpRoutedProgram` controlling that, defaulting to printing it so the test's
output is unchanged.

The JSON object must contain a key for each routing parameter under an `options`
object, an integer `num_swaps`, an object `initial_sites` and an object
`final_sites` each mapping program qubit name to site index, and a string
`openqasm3`. Emit it with `llvm::json::OStream` so that string escaping is
handled correctly.

If the pass fails, print a diagnostic to standard error and return a non-zero
exit status, so that a driving script can detect failure.

## Concrete Steps

Work from the repository root at `/Users/yudong/Documents/projects/mqt.core`.

Before editing, build and run the existing test to establish a baseline:

    cmake --preset release
    cmake --build --preset release --target mqt-core-mlir-unittest-rydberg-ions
    ./build/release/mlir/unittests/Dialect/QCO/Transforms/RydbergIons/mqt-core-mlir-unittest-rydberg-ions

Expect a transcript ending with a line reporting two tests passed. Record that
number.

After the edits, rebuild both the test and the new tool:

    cmake --build --preset release --target mqt-core-mlir-unittest-rydberg-ions
    cmake --build --preset release --target mqt-rydberg-route

Run the test again and confirm the same two tests pass. Then run the tool:

    ./build/release/bin/mqt-rydberg-route --seed=1

Expect output beginning with the banner and the `options:` line and ending with
the last line of OpenQASM 3, with nothing after it.

Compare two seeds:

    ./build/release/bin/mqt-rydberg-route --seed=1 | grep -c '^swap'
    ./build/release/bin/mqt-rydberg-route --seed=2 | grep -c '^swap'

Expect two integers in the seventy to ninety range that generally differ.

Check the JSON path parses:

    ./build/release/bin/mqt-rydberg-route --emit=json | python3 -m json.tool | head -20

## Validation and Acceptance

The first acceptance criterion is that the existing GoogleTest binary reports
the same number of passing tests, with the same assertions, as it did before the
change. This proves the extraction preserved behaviour.

The second is that the tool's default invocation, with no flags, produces a
report whose `options:` line is identical to the one the test prints, except
that the tool's report must not be followed by a trailing banner. Verify by
running both and comparing the `options:` lines with `diff`.

The third is that the report the tool emits is consumed without error by the
downstream parser. Verify by writing the tool's output to a file and running,
from `/Users/yudong/Documents/projects/qec-rydberg-ions`:

    uv run python -c "import sys; sys.path.insert(0,'src'); import mapper_output; p=mapper_output.parse_mapper_output('/tmp/route.in'); print(p.num_physical_qubits, len(p.gates))"

Expect it to print a qubit count of twelve and a gate count, and to raise no
exception. This is the criterion that actually matters, because it is the reason
the tool exists.

The fourth is that `--emit=json` produces output that `python3 -m json.tool`
accepts, and whose `num_swaps` value equals the number of lines beginning with
`swap` in the corresponding `in`-format output for the same seed.

The fifth is that the reported initial layout round-trips. Run the tool once,
read the `initial-layout:` line from its output, and run the tool again passing
that value to `--initial-layout` together with `--pin-initial-layout`. The
second run's `initial-layout:` line must be identical to the first's. Its SWAP
count will generally differ, because routing is not reproducible; that is
expected and is not a failure of this criterion. This check requires
`.agent/plans/initial-layout-option.md` to have landed; if it has not, verify
instead that the reported layout is a permutation of the site indices and record
in `Outcomes & Retrospective` that the round-trip is untested.

The sixth is that the defaults are genuinely defaults. Run the tool with
`--circuit=bacon-shor` explicitly and confirm the `options:` line is identical
to the no-flags run. Run it with `--circuit=nonexistent` and confirm it exits
non-zero having listed the available names. Run it with `--coupling-map`
supplied but `--num-sites` omitted and confirm it exits non-zero with a message
saying both are required together.

Add no new GoogleTests. The tool's correctness is established by the existing
test continuing to pass over the shared code, plus the parser round-trip above.

## Idempotence and Recovery

All build and run steps are repeatable. The tool writes only to standard output
or to a file the user names, and mutates nothing in the repository.

The risky step is the extraction, because it moves a large amount of code
between translation units. Do it in the order given: move the code and get the
test compiling and passing again *before* creating the tool. If the test fails
after the move, the fault is in the extraction, not in the tool, and the
recovery path is to compare the moved definitions against their originals in
version control.

Note that because routing is not reproducible across process launches, a failing
comparison between two runs is not by itself evidence of a bug. When you need a
deterministic comparison, compare the `options:` lines and the structure of the
output, not the specific SWAP sequence.

## Artifacts and Notes

The existing test's own comment at its options block reads that the constants
are there to be edited and the test rerun, which is precisely the workflow this
tool replaces. Quote from the file, as indented text:

    // Edit these to rerun the mapping pass below with different
    // `MappingPassOptions`

A checked-in example of the report this tool must produce, with the MLIR section
and trailing noise already stripped, is
`/Users/yudong/Documents/projects/qec-rydberg-ions/scripts/test_cases/5.in`.

A note for whoever later adds the ability to read a circuit from a file rather
than build it in C++, which is the intended eventual direction and is
deliberately not part of this plan. The work is small, because every piece
already exists in this repository and is already wired together in
`mlir/tools/mqt-cc/mqt-cc.cpp`. That file loads an OpenQASM 3 file at
approximately line 264 by handing a `llvm::SourceMgr` to
`qc::translateQASM3ToQC`, loads a textual MLIR file at approximately line 284
with `parseSourceFile<ModuleOp>`, and converts a QC-dialect module into the QCO
dialect the mapping pass needs at approximately line 514 by running
`createQCToQCO()`. Adding a file source to this tool is therefore adding one
more entry at the same decision point as the circuit registry, not new
machinery.

Two things are not free, and are the reason this is written down rather than
assumed. First, the report identifies each program qubit by a name such as
`data0` or `ancilla2`, and those names come from the C++ builder through
`struct QubitTrace`; a circuit read from a file has registers and indices
instead, so a rule for deriving those names must be chosen, and the downstream
Python parser depends on them because it separates data from auxiliary qubits by
name prefix. Second, this pipeline relies on the three-qubit operations `ccx`
and `ccz` surviving as native operations rather than being decomposed, and
whether they survive the OpenQASM 3 import path intact must be verified rather
than assumed. Neither is large, but neither is nothing, and a reader who
believes file input is purely mechanical will be surprised by the first.

## Interfaces and Dependencies

No new third-party dependencies. The JSON emitter is `llvm::json::OStream` from
`llvm/Support/JSON.h`, already available.

At the end of this milestone the following must exist. A CMake target
`MQTRydbergBaconShor` exposing, through
`mlir/include/mlir/Dialect/QCO/Testing/RydbergIons/BaconShor.h`, at least these
declarations: a function constructing a `CompilerTarget` from a site count, an
edge list and a list of native operation names, together with a zero-argument
wrapper returning today's twelve-site Rydberg-ion target; a registry of named
circuit builders containing at least the entry `bacon-shor`, each entry carrying
a builder that populates a given `ModuleOp`, a default qubit type label string
and a description; a function running the mapping pass with a given target and
`MappingPassOptions`; and the report printer, taking an output stream, the
module, the options, the qubit traces, the site map, the OpenQASM 3 text, and a
boolean controlling the trailing banner. A CMake target `mqt-rydberg-route`
producing an executable of that name under `build/<preset>/bin/`, accepting a
flag for every `MappingPassOptions` field plus `--circuit`, `--num-sites`,
`--coupling-map`, `--native-gates`, `--emit` and `-o`, and emitting an
`initial-layout:` line in the `in` format and an `initial_layout` key in the
`json` format. The existing target `mqt-core-mlir-unittest-rydberg-ions` must
continue to exist and pass.

The library's name retains the word `BaconShor` for continuity with the file
layout described above even though it now holds a registry. If a second circuit
is added, renaming it to something like `MQTRydbergCircuits` is a reasonable
follow-up and should be done in one commit that changes nothing else.
