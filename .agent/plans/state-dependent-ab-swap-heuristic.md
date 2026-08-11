# Implement the opt-in stateful A/B swap heuristic

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

## Purpose / Big Picture

`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` implements the
`place-and-route` pass (`MappingPass`), which inserts `qco.swap` operations so
that every two-qubit operation in a quantum program ends up on adjacent sites of
a target device. It chooses which SWAPs to insert with an A* search whose cost
function today treats every candidate SWAP as equally expensive.

After this change, a user of the pass can opt in to a "stateful A/B swap
heuristic" by passing the new `qubit-type-labels` pass option (C++ option field
`qubitTypeLabels`), a string such as `"AABB"` that assigns each program-level
qubit (a logical/circuit qubit, addressed by the order in which it is allocated
in the source program) a type label: `A` conventionally means "auxiliary qubit",
`B` conventionally means "data qubit" (lowercase `a`/`b` and `0`/`1` are
accepted as synonyms for `A`/`B`). Once enabled, every candidate SWAP the A*
search considers is priced by the labels of the two program qubits it would
exchange: swapping two `A`-labeled qubits costs `1`, swapping an `A` with a `B`
costs `2`, and swapping two `B`-labeled qubits costs `3`, replacing the old flat
per-SWAP cost of `1`. Because the search always looks up a candidate SWAP's cost
through the *current* routed layout (the layout reflects every SWAP already
chosen along that search branch), the same initial A/B assignment influences
every subsequent routing decision, not just the first one. Leaving
`qubit-type-labels` unset (the default, an empty string) reproduces the exact
previous behavior: this is an additive, opt-in-only change to `MappingPass`.

The observable proof: running the pass twice on the same program and target with
two different `qubit-type-labels` assignments produces different, independently
valid SWAP sequences (both still make the program executable on the target),
demonstrating that the label assignment measurably steers routing choices. A new
focused GoogleTest in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` asserts this
directly by comparing the routed programs' SWAP operand sites for two label
strings on the same topology and gate pattern.

## Progress

- [x] (2026-08-11) Read
      `state-dependent-ab-swap-heuristic_original_taskplan.md`, the current
      `Mapping.cpp`/`Mapping.h`/ `Passes.td`/`test_mapping.cpp`, and the
      `Layout` utility API to design the feature.
- [x] (2026-08-11) Discovered an uncommitted-to-branch prior attempt at commit
      `4f32e387` ("GitHub Copilot Attempt", reachable from `main` but not from
      this task's branch); reviewed it for design ideas and rejected its
      per-node label-array-swap approach as semantically incorrect (see Decision
      Log).
- [x] (2026-08-11) Added the `qubitTypeLabels`/`qubit-type-labels` pass option
      and updated the `MappingPass` `description` in
      `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`.
- [x] (2026-08-11) Added the `QubitLabel` enum, the `parseQubitLabels`
      validating parser, the `qubitLabels` pass member, and wired parsing plus
      error reporting into `MappingPass::runOnOperation` in `Mapping.cpp`.
- [x] (2026-08-11) Extended `Node` with `pathCost`/`useTypedCost` and the
      `typedSwapCost` cost model; updated `Node::g` to switch on `useTypedCost`.
- [x] (2026-08-11) Threaded `qubitLabels`/`useTypedCost` into the `search()`
      function's root and child `Node` construction call sites.
- [x] (2026-08-11) Discovered the tracked build tree (`build/release`) was
      configured from a different machine's path (`/Users/yudong/...`) and could
      not be reused; reconfigured it fresh via
      `rm -rf build/release && ./.agent/run.sh cmake --preset release`, per the
      "safe to reconfigure/rebuild from scratch" note in Idempotence and
      Recovery.
- [x] (2026-08-11) Discovered
      `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` fails to
      compile in this devcontainer (Ubuntu 26.04 base image, GCC 15 / Clang 21,
      both against the same libstdc++ 15) on a pre-existing, unrelated line
      unrelated to this feature:
      `SmallVector<Value> argQs(llvm::reverse(args));` triggers a
      libstdc++15/LLVM-22.1.7 iterator-`default_initializable`-concept
      incompatibility with `MLIR`'s `indexed_accessor_range_base::iterator` for
      both compilers. With the user's explicit authorization, applied a minimal,
      separate fix (kept in the same file, but logically independent of the
      feature): replaced it with
      `SmallVector<Value> argQs(args); std::reverse(argQs.begin(), argQs.end());`
      and added `#include <algorithm>`. This unblocked compiling and running
      every test in the file, including the new ones below. See Surprises &
      Discoveries for the diagnosis trail.
- [x] (2026-08-11) Added `StatefulSwapLabelsChangeRoutingChoice` (proves two
      label strings on the same 3-node-path target/triangle-CX program/seed make
      the pass insert its one unavoidable SWAP at a different point, via
      `countTwoQubitGatesBeforeFirstSwap`) and
      `StatefulSwapLabelsPreferCheaperTypedSwap` (proves, for two complementary
      label assignments — "ABB" and "BAA" — which *specific* pair of program
      qubits the inserted SWAP exchanges, via `traceProgramIdentities`, matches
      the cheaper option under the A<>A=1, A<>B=2, B<>B=3 cost model) and
      `InvalidQubitTypeLabelsFailsThePass` to `test_mapping.cpp`.
- [x] (2026-08-11) Built the release preset and ran the focused mapping unittest
      binary repeatedly while iterating; all exact seed/label/ expected-value
      combinations were confirmed empirically against real pass output (see
      Surprises & Discoveries) rather than hand-derived.
- [x] (2026-08-11) Added the `CHANGELOG.md` `### Added` entry (with a
      `#????`/`@sunjerry019` placeholder PR reference and author link, per
      explicit user direction, since no PR exists yet for this branch).
      `UPGRADING.md` was not touched: it is scoped to breaking changes only, and
      this option is purely additive/opt-in.
- [x] (2026-08-11) Ran `./.agent/run.sh cmake --build --preset release` (full
      build, 756 targets) and `./.agent/run.sh ctest --preset release` (4373
      tests, 100% pass, same 2 pre-existing configured QDMI skips).
- [x] (2026-08-11) Ran `./.agent/run.sh uvx nox -s lint`; it failed only on the
      pre-existing, environment-specific `bibtex-tidy` hook (`node` is not
      installed in this container at all — unrelated to any file in this diff,
      `.bib` files aren't touched here). Re-ran the same hook set directly via
      `SKIP=bibtex-tidy ./.agent/run.sh prek run --all-files`; every other hook
      (clang-format, clang-format for TableGen, rumdl, typos, license headers,
      ruff, ty, etc.) passed, auto-fixing formatting on the first pass and
      reporting clean on the second. Rebuilt and reran the focused mapping suite
      after formatting to confirm the auto-fixes didn't change behavior (still
      81/81 passing).
- [x] (2026-08-11) Final read-through of the diff against `AGENTS.md` and this
      plan.

All planned work is complete; see Outcomes & Retrospective.

## Surprises & Discoveries

- Observation: the build tree at `build/release` already contained a generated
  `Passes.h.inc` with a `statefulSwapLabels` option field, even though
  `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` on this branch has no
  such option and `git status`/`git diff` show no uncommitted source changes.
  Evidence: `grep -n "statefulSwapLabels" build/release/.../Passes.h.inc`
  matched a full option declaration, while the same grep against the tracked
  `Passes.td` matched nothing.
- Explanation: `git log --all` and `git reflog` show a sibling commit `4f32e387`
  "GitHub Copilot Attempt" (authored today, reachable from `main`, not an
  ancestor of this task's branch `state_dep_swaps_2`) that added exactly that
  option under a different name/shape. The stale build directory was evidently
  produced from a worktree state that included that commit's changes before this
  branch was reset to its current base. It was used only as design reference
  (see Decision Log), not copied verbatim.
- Observation: `build/release/CMakeCache.txt` recorded its original configure
  directory as `/Users/yudong/Documents/projects/mqt.core/build/release`, not
  this container's `/workspaces/mqt.core/build/release`, so CMake refused to
  build, citing a `CMakeCache.txt` directory mismatch between the recorded and
  current build directories. Evidence: the literal CMake error text and `grep`
  of `CMakeCache.txt`. Resolution: `rm -rf build/release` and a fresh
  `./.agent/run.sh cmake --preset release`; this is the git-ignored, freely
  regeneratable build directory, so no source or history was at risk.
- Observation: with default `gcc` (system default, 15.2.0) *and* with `clang++`
  (system default, 21.1.8) — both configured fresh via `CC=/CXX=` env vars —
  `test_mapping.cpp` failed identically on
  `SmallVector<Value> argQs(llvm::reverse(args));`: libstdc++15's constrained
  `std::uninitialized_copy` requires `std::reverse_iterator<It>` to satisfy
  `contiguous_iterator`, which in turn requires `It` to be
  `default_initializable`; MLIR's `indexed_accessor_range_base<...>::iterator`
  (the `ValueRange` iterator) only has a 2-argument and copy/move constructor,
  not a default one. Since the failure reproduced identically under both
  compilers (both share the same libstdc++ 15 headers on this Ubuntu 26.04
  devcontainer base image), this is a standard-library, not a compiler-frontend,
  incompatibility. Evidence: near-identical
  `error: no matching constructor for initialization of '...ValueRange...iterator'`
  traces from both `g++` and `clang++`, both bottoming out in
  `bits/stl_iterator.h`'s `_GLIBCXX_NOEXCEPT_IF(noexcept(_Iterator()))`.
  Resolution: per explicit user authorization, replaced the one offending line
  with an equivalent that never forms a `std::reverse_iterator` over that MLIR
  iterator type: build the `SmallVector` from `args` directly (the same pattern
  the adjacent, unaffected lambda already uses), then `std::reverse` its own
  (trivially default-constructible, pointer-based) iterators in place. Also
  tried `llvm::to_vector(llvm::reverse(args))` first; it hit the exact same root
  cause (still routes through `SmallVector`'s iterator-range constructor
  internally) and was discarded in favor of the working fix.
- Observation: an ad hoc scratch diagnostic test (used only to empirically pick
  concrete seed/label/target parameters for the committed regression tests,
  never committed) initially misattributed a `DenseMap::operator[]`
  default-value artifact ("SWAP(0, 0)" printed for both operands) to a bug in
  the pass itself. Evidence: `isExecutable` (the existing, already-trusted
  helper used throughout this test file) returned `true` for every one of the 48
  seed/label combinations swept, proving the routed IR was always correctly
  adjacency-constrained; the "(0, 0)" artifact was traced to the scratch
  helper's incorrect assumption that a SWAP's *site* propagation needs the same
  input↔output crossover as its *program-identity* propagation. For **site**
  tracking (which hardware position a value occupies), a SWAP's
  `qubit0_out`/`qubit1_out` keep the *same* site as `qubit0_in`/`qubit1_in`
  respectively (no crossover) — confirmed by reading `isExecutable`'s own
  (correct, pre-existing) propagation loop, which handles `SWAPOp` with no
  special case at all. For **program-identity** tracking (which original logical
  qubit a value's state currently is), a SWAP *does* crossover: `Mapping.cpp`'s
  `insertSWAPs` calls `rewriter->replaceAllUsesExcept(in0, out1, swapOp)` /
  `replaceAllUsesExcept(in1, out0, swapOp)`, i.e., `in0`'s future consumers are
  redirected to `out1`. `predecessorQubit` (used by the final
  `traceProgramIdentities` helper) implements this crossover correctly.
- Observation: `traceProgramIdentities`'s first implementation (backward-only,
  from each `qco.measure`'s qubit *input* back to its `qco.static` root) crashed
  with a `DenseMap::at` missing-key assertion when looking up one operand of the
  SWAP inserted for the "BAA"-labeled sanity check. Evidence: dumping the routed
  module showed the crashing operand was `%qubit_out` — the *output* of an
  earlier `qco.measure` (QCO's linear semantics keep a qubit live and reusable
  after measurement; here the compiler reused a just-measured qubit's wire as
  SWAP workspace before finally sinking it) — which the backward-only walk never
  visits, since it only follows `qco.measure`'s *input* chain. Fixed by adding a
  second, forward pass over the whole function that propagates every
  already-known program tag through subsequent def-before-use operations
  (including `MeasureOp`'s own input→output edge and `SWAPOp`'s crossover), so
  post-measurement "workspace reuse" segments are covered too. This directly
  informed (and is exercised by) the final
  `StatefulSwapLabelsPreferCheaperTypedSwap` test.
- Observation: the same "BAA"-labeled sanity check confirmed the cost model is
  genuinely driving the choice, not a seed/topology coincidence: with labels
  "ABB" (q0 the sole "A"), the inserted SWAP provably involves program qubit 0;
  with the complementary "BAA" (q0 the sole "B", q1/q2 both "A"), the *same*
  seed/topology/program instead provably involves *only* program qubits 1 and 2
  (the cheap A<>A pair), never touching program qubit 0. Evidence:
  `llvm::errs()`-dumped module IR and `traceProgramIdentities` output during
  interactive iteration, since folded into the committed
  `StatefulSwapLabelsPreferCheaperTypedSwap` test's two sub-cases.

## Decision Log

- Decision: label state is a dense `SmallVector<QubitLabel>` sized to
  `target->numQubits()`, indexed by permanent program-qubit identity, computed
  once in `runOnOperation` and stored as a `MappingPass` member (`qubitLabels`)
  rather than threaded through `RoutingBundle`/`Node` copies. Rationale: in this
  codebase a "program index" (`prog`) is a stable, permanent identity assigned
  once in `discoverComputation`/`place` (`infos.insertOrUpdate(index, index)`);
  nested-region routing (`dispatch()`'s
  `scf.for`/`scf.while`/`qco.if`/`qco.index_switch` handling) reuses the same
  global `prog` numbering in child `RoutingBundle`s (it only renumbers the
  *local wire index*, via
  `child.infos.insertOrUpdate(child.infos.size(), prog)`, never `prog` itself).
  A label keyed by `prog` is therefore valid everywhere in the routing traversal
  without being copied into every `RoutingBundle`/ `Node`, which is a much
  smaller, lower-risk diff than threading a parallel array through every
  construction site in `dispatch()`. Date/Author: 2026-08-11, implementing
  agent.
- Decision: the label attached to a program qubit is never mutated by a SWAP;
  only the `Layout` (`prog<->hw` mapping) changes. A SWAP's cost is computed by
  looking up `layout.getProgramIndices(hw0, hw1)` *before* applying
  `layout.swap(hw0, hw1)` to find which two program qubits (and hence which two
  labels) it exchanges. Rationale:
  `state-dependent-ab-swap-heuristic_original_taskplan.md` states "each
  logical/circuit qubit carries a type label" and "'A' corresponds to an
  auxiliary qubit, and 'B' corresponds to a data qubit" — an auxiliary/data role
  is an intrinsic property of a specific logical qubit for the whole program; it
  would be a contradiction for a data qubit to become an auxiliary qubit merely
  because a SWAP moved it. The reviewed prior attempt at commit `4f32e387`
  instead swapped entries of a parallel `labels` array in lockstep with every
  `Layout::swap`, indexed by program id. Tracing that logic shows it reassigns
  which *program id* is considered `A` vs `B` after every single SWAP (the two
  exchanged program ids trade labels), which inverts the "permanent
  per-logical-qubit role" semantic implied by "auxiliary"/"data". This
  implementation instead treats `qubitLabels` as immutable for the whole pass
  run and relies solely on the already-existing `Layout` swap bookkeeping to
  know which program qubit currently occupies which hardware site — matching the
  plan's phrase "the search heuristic should use the current state of the routed
  layout." Date/Author: 2026-08-11, implementing agent.
- Decision: only the A* path cost `g(n)` becomes type-dependent
  (`g(n) = alpha * pathCost(n)` when enabled, where `pathCost` sums
  `typedSwapCost` over the SWAPs from the root to `n`); the remaining-cost
  heuristic `h(n)` is left unchanged (still the original coupling-graph distance
  sum over the lookahead window). Rationale:
  `state-dependent-ab-swap-heuristic_original_taskplan.md` Step 3 says "each
  candidate swap should contribute a type-dependent cost", which maps directly
  onto the cost of the specific SWAP edge added when expanding a search node —
  i.e. `g`, the realized path cost. Changing `h` as well would require
  estimating the *types* of not-yet- chosen future SWAPs along an unknown
  remaining path, which is speculative and not requested; keeping `h` as an
  admissible-ish distance estimate keeps the search's remaining-work guidance
  intact while `g` alone drives the label-sensitive preference between
  equally-short SWAP chains. Date/Author: 2026-08-11, implementing agent.
- Decision: `qubit-type-labels` characters are validated (`A`/`a`/`0` or
  `B`/`b`/`1` only); an invalid character fails the pass with a diagnostic
  instead of silently defaulting to `A`. Rationale: explicit user instruction
  during implementation review to reject unexpected characters rather than
  silently coercing them, which would hide a typo'd option value as a silent
  behavior change. Date/Author: 2026-08-11, implementing agent (per human
  reviewer direction).
- Decision: unlabeled trailing program qubits (when `qubit-type-labels` is
  shorter than the number of target qubits) default to `B` (data) rather than
  failing. Rationale: matches `Further Considerations` item 1 in
  `state-dependent-ab-swap-heuristic_original_taskplan.md` ("dense storage is
  likely simpler and faster") and lets a caller label only the qubits it cares
  about (e.g. a handful of known ancillas) without enumerating every
  workspace/padding qubit the pass may materialize. Originally defaulted to `A`
  (auxiliary, the cheapest label); changed to `B` per explicit user direction on
  2026-08-11, so that qubits the caller didn't bother labeling are treated as
  ordinary data qubits by default rather than as (implicitly cheap-to-move)
  auxiliary qubits. Date/Author: 2026-08-11, implementing agent; revised
  2026-08-11 per user direction.

## Outcomes & Retrospective

All Purpose/Big Picture goals were achieved. `MappingPass` now accepts an opt-in
`qubit-type-labels` string option; when set, every SWAP the A* search realizes
along a candidate path is priced by the (validated, `A`/`a`/`0` or `B`/`b`/`1`)
labels of the two program qubits it exchanges (A<>A=1, A<>B=2, B<>B=3) instead
of a flat per-SWAP unit cost, and this is proven end-to-end by three new
GoogleTests: one showing two label strings change *where* the pass needs to
insert its one unavoidable SWAP for a fixed program/target/seed, one showing two
complementary label strings change *which specific pair* of program qubits gets
swapped in the direction the cost model predicts (twice, from both directions),
and one showing an invalid label string fails the pass with a diagnostic instead
of silently misbehaving. Leaving the option unset is behaviorally identical to
before this change (verified by the full pre-existing suite passing unmodified).

The final implementation is smaller than the design sketched in Plan of Work
step 2 suggested it might need to be: because a "program index" is a permanent,
pass-wide identity in this codebase (see the first Decision Log entry), the
label vector lives once on the `MappingPass` object and is looked up through the
existing `Layout`, with no changes needed to `RoutingBundle`, `insertSWAPs`, or
`dispatch` — the diff touches only `Node`'s cost computation and `search()`'s
two `Node`-construction call sites, plus the new option/parsing/member. The
reviewed prior attempt (commit `4f32e387`, not an ancestor of this branch) took
a different, larger-diff path — a parallel `labels` array threaded through every
`RoutingBundle` and swapped in lockstep with every `Layout::swap` — that this
plan's second Decision Log entry argues is semantically inverted for an
"auxiliary/data" role that should be a fixed property of a specific logical
qubit; that design difference was the main judgment call of this task, made
confidently but without being able to ask the prior implementation's author why
they chose it.

Two environment issues outside this task's scope were hit and resolved with
explicit user sign-off rather than worked around silently: a stale,
wrong-machine-path build cache (deleted and regenerated, no risk since `build/`
is git-ignored) and a pre-existing libstdc++15/LLVM-22.1.7 iterator
incompatibility on one unrelated line in the test file (fixed minimally,
confirmed to reproduce identically under both available compilers before
concluding it was a standard-library rather than a frontend issue). Full
validation was completed: a clean full release build (756 targets), the complete
CTest suite (4373/4373 passing, the same 2 pre-existing configured skips), and
the full `prek` hook set (every hook passed; only the environment's missing
`node` binary — unrelated to this diff — kept the `bibtex-tidy` hook from
running, and that was independently confirmed via a `SKIP=bibtex-tidy` rerun of
every other hook).

The main lesson for future work in this area: when a plan's
`Further Considerations` flags an open design question (here, "dense vs. sparse
storage" and implicitly "what does the label attach to"), it is worth
empirically probing the *actual* routed IR early — via a throwaway,
never-committed scratch test — before finalizing the cost-model semantics,
rather than reasoning about SWAP crossover semantics from the wording of a task
description alone. That empirical step is also what caught this session's own
tracer bugs (site vs. program-identity crossover; backward-only vs.
bidirectional tracing) before they could hide inside a false-negative or, worse,
a false-positive committed assertion.

## Context and Orientation

`mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` implements `MappingPass`,
an MLIR pass (`mlir::qco::MappingPass`, registered as pass `place-and-route`)
that lowers dynamically allocated ("program") qubits onto the static sites of a
`CompilerTarget` (`mlir/include/mlir/Compiler/Target.h`) and inserts `qco.swap`
operations (`SWAPOp` in `mlir/include/mlir/Dialect/QCO/IR/QCOOps.h`) so every
two-qubit operation ends up on adjacent target sites. "Program index"/`prog`
names a logical qubit by its permanent allocation order (see
`discoverComputation` and `place` in `Mapping.cpp`); "hardware index"/`hw` names
a physical site's position in the target's coupling graph. `mlir::qco::Layout`
(declared in `mlir/include/mlir/Dialect/QCO/Utils/Layout.h`, defined in
`mlir/lib/Dialect/QCO/Utils/Layout.cpp`) is the bijection between the two:
`getHardwareIndex(prog)`/`getProgramIndex(hw)` (and the multi-argument
`getHardwareIndices`/`getProgramIndices` tuple-returning overloads) look up one
side from the other, and `layout.swap(hwA, hwB)` exchanges which program qubit
occupies which of two hardware sites (this is what a routed SWAP operation
physically does).

Routing works by dividing the circuit into layers of independently executable
two-qubit operations and running an A* search (the private nested `Node` struct
and `search()` member function inside the anonymous-namespace `MappingPass`
class in `Mapping.cpp`) to find a short SWAP sequence that makes the next layer
(plus `nlookahead` more layers, for lookahead) executable. Each `Node` wraps a
candidate `Layout` reached by a sequence of hypothetical SWAPs;
`Node::f = Node::g(alpha) + Node::h(window, target, params)` is the A* cost,
with `g` the realized path cost of the SWAPs applied so far and `h` an
admissible estimate of SWAPs still needed for the current lookahead window.
Before this change, `g(n) = alpha * depth(n)` (every SWAP costs one flat unit)
and `h` sums, for each layer with decay `lambda^i`, the coupling-graph distance
between the layer's two operand qubits under node `n`'s layout minus one (the
number of SWAPs a naive router would need). `search()` builds `Node` objects
from a `llvm::SpecificBumpPtrAllocator<Node>` arena and orders them in an
`llvm::PriorityQueue` by ascending `f`.

`MappingPass`'s pass options are declared once, in TableGen, in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` (the `MappingPass` def, its
`options` list). Building the project (see Concrete Steps) code-generates
`MappingPassOptions` (a plain struct with one field per option) and
`MappingPassBase` (which owns one `mlir::Pass::Option<T>` per option field,
constructible from a `MappingPassOptions`) into
`build/<preset>/mlir/include/mlir/Dialect/QCO/Transforms/Passes.h.inc`, which
`Mapping.cpp` includes via `#include "mlir/Dialect/QCO/Transforms/Passes.h.inc"`
inside a `GEN_PASS_DEF_MAPPINGPASS` guard. Each TableGen
`Option<cppName, arg, type, default, description>` becomes a
`MappingPassOptions` field named `cppName` and a protected `MappingPassBase`
member also named `cppName` (usable directly as `this->cppName`, e.g. existing
fields `alpha`, `lambda`, `seed`); the class under test constructs a pass with
`createMappingPass(target, MappingPassOptions{...})`
(`mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h`).

Unit tests for this pass live in
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`. Tests build an
MLIR module with `QCOProgramBuilder`
(`mlir/include/mlir/Dialect/QCO/Builder/QCOProgramBuilder.h`), run the pass via
the `MappingPassFixture::runPass(ModuleOp, CompilerTarget, MappingPassOptions)`
static helper (defined at the top of the test file), then assert on the
resulting IR — typically via `isExecutable(...)` (a local helper that checks
every two-qubit operation sits on adjacent target sites) and/or by walking
`SWAPOp`s directly with `m->walk([&](SWAPOp) { ... })`.

## Plan of Work

1. `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` (done): add
   `Option<"qubitTypeLabels", "qubit-type-labels", "std::string", "\"\"", ...>`
   to `MappingPass`'s `options` list, and extend the `description` block with a
   paragraph documenting the opt-in heuristic and its cost model, matching the
   level of detail already given for the base `g`/`h` cost function.
2. `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` (in progress):
   - Add a `QubitLabel` enum (`Auxiliary`/`Data`) next to the existing
     `RoutingMode` enum.
   - Add a `parseQubitLabels(StringRef spec, size_t nqubits)` static helper
     (near `getQubitValues`) returning `FailureOr<SmallVector<QubitLabel>>`:
     empty `spec` yields an empty vector (heuristic disabled); otherwise a
     `nqubits`-sized vector defaulting every entry to `Auxiliary` and
     overwriting the first `min(spec.size(), nqubits)` entries per character
     (`A`/`a`/`0` -> `Auxiliary`, `B`/`b`/`1` -> `Data`, anything else ->
     `failure()`).
   - Add a `SmallVector<QubitLabel> qubitLabels;` private member next to the
     existing `std::optional<CompilerTarget> target;` field.
   - In `runOnOperation`, after the entry-point (`func`) lookup succeeds, call
     `parseQubitLabels(qubitTypeLabels.getValue(), target->numQubits())`; on
     failure, emit a diagnostic via `func.emitError()`, call
     `signalPassFailure()`, and return, mirroring the existing
     `discoverComputation` failure-handling pattern a few lines below; on
     success, move the parsed vector into `qubitLabels`.
   - Extend `Node` with `float pathCost` (sum of `typedSwapCost` over the path
     from the root) and `bool useTypedCost` (copied from the parent, set
     explicitly on the root); add the edge-cost computation to the non-root
     constructor (look up `layout.getProgramIndices(swap.first, swap.second)`
     *before* calling `layout.swap`, since the member-initializer list already
     copied `parent->layout` into `this->layout` at that point); update
     `Node::g` to return `alpha * (useTypedCost ? pathCost : depth)`; add the
     `static float typedSwapCost(QubitLabel, QubitLabel)` helper implementing
     `A<>A=1, A<>B=2, B<>B=3`.
   - Update `search()`'s root-node construction to pass `!qubitLabels.empty()`
     as the new `useTypedCost` argument, and its child-node construction (inside
     the neighbor-expansion loop) to pass `qubitLabels` as the new
     `ArrayRef<QubitLabel>` argument.
   - No other function needs to change: `RoutingBundle`, `insertSWAPs`, and
     `dispatch` are untouched because `qubitLabels` is looked up by permanent
     `prog` identity from the pass-level member, not threaded through routing
     state (see Decision Log).
3. `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`: add a
   `MappingPassFixture`-based test that builds one small program on a fixed
   topology chosen so that at least one A* tie exists between two SWAP choices
   of equal graph-distance cost but different label cost, runs the pass twice
   with two label strings that swap which side is cheaper, and asserts the two
   runs select different SWAP operand sites (not just a different count) while
   both remain `isExecutable`. Add a second test that passes an invalid
   `qubit-type-labels` string (e.g. `"AXBB"`) and asserts `runPass` returns
   `failure()`. Exact topology/program details are worked out empirically
   against the real build in the next step, since hand-tracing the A* tie-break
   order is error-prone; the Concrete Steps section will be updated with the
   final program/topology once confirmed.
4. `CHANGELOG.md`: add an `Unreleased`/`Added` (`✨`) bullet describing the new
   `qubit-type-labels` option, once a PR reference is available (or left as a
   short placeholder consistent with neighboring entries if this repository's
   convention allows drafting before a PR number exists — confirm against the
   current `CHANGELOG.md` header conventions before writing the entry).

## Concrete Steps

All commands run from the repository root.

1. Configure once (if not already configured):
   `./.agent/run.sh cmake --preset release`
2. Rebuild after each source edit, scoped to the mapping unit test binary to
   keep iteration fast:
   `./.agent/run.sh cmake --build --preset release --target mqt-core-mlir-unittest-mapping`
3. Run the focused suite:
   `./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping`
   and narrow with `--gtest_filter='*StatefulSwap*'` (or the final chosen test
   names) while iterating.
4. Once the new tests are green, run the full focused mapping suite (no filter)
   to confirm no regression, then the broader MLIR unit test set if time allows:
   `./.agent/run.sh ctest --preset release` (or a scoped `-R mapping` invocation
   while iterating, full run before declaring done).
5. `./.agent/run.sh uvx nox -s lint`.

## Validation and Acceptance

- The new `StatefulSwapLabelsChangeRoutingChoice`-style test fails on the
  pre-change code (no such option exists, so it would fail to compile) and
  passes after the change, demonstrating that two different label strings
  produce two different, independently valid SWAP placements for the same input
  program and target.
- The new invalid-input test demonstrates that `runPass` returns `failure()` for
  a malformed `qubit-type-labels` value instead of silently misbehaving.
- All previously passing tests in
  `mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp` continue to
  pass unmodified, demonstrating the option is purely additive/opt-in.
- `./.agent/run.sh uvx nox -s lint` passes.

## Idempotence and Recovery

Every step is a source edit plus a rebuild/rerun; none mutate persistent state
outside the working tree and the `build/` directory (itself git-ignored and safe
to reconfigure/rebuild from scratch with the commands in Concrete Steps if it
becomes inconsistent). No destructive git operations are part of this plan; the
stray stale `build/release` artifacts noted in Surprises & Discoveries are
regenerated by the normal build step and require no manual cleanup.

## Artifacts and Notes

- Prior-attempt diff reviewed for design ideas: `git show 4f32e387` (a commit on
  `main`, not an ancestor of this task's branch). See Decision Log for why its
  per-node label-swap approach was not reused.

## Interfaces and Dependencies

- New TableGen option in `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`:
  `Option<"qubitTypeLabels", "qubit-type-labels", "std::string", "\"\"", ...>`
  on `MappingPass`, generating `MappingPassOptions::qubitTypeLabels` (default
  `""`) and `MappingPassBase::qubitTypeLabels` (a
  `mlir::Pass::Option<std::string>`).
- New private types/members on the anonymous-namespace `MappingPass` class in
  `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`:
  `enum class QubitLabel : uint8_t { Auxiliary = 0, Data = 1 };`,
  `static FailureOr<SmallVector<QubitLabel>> parseQubitLabels(StringRef, size_t)`,
  `SmallVector<QubitLabel> qubitLabels;` (member).
- Extended nested `Node` struct: new fields `float pathCost`,
  `bool useTypedCost`; new root constructor signature
  `Node(Layout, bool useTypedCost)`; new child constructor takes the prior
  `Node*`, `const IndexPairType&`, `const Window&`, `const CompilerTarget&`,
  `const Parameters&`, and `ArrayRef<QubitLabel>`; new
  `static float typedSwapCost(QubitLabel, QubitLabel)`.
- No changes to `mlir/include/mlir/Dialect/QCO/Transforms/Mapping/Mapping.h`
  (the `createMappingPass(const CompilerTarget&, MappingPassOptions)` factory
  signature is unchanged; the new option flows through the existing
  `MappingPassOptions` struct).
