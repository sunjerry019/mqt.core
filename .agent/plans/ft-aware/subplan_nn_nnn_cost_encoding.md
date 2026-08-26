# Subplan (current focus): encoding NN/NNN edge cost into the mapping pass

Status: brainstorm, pending review — not an ExecPlan yet (see
`.agent/PLANS.md`). Options below are presented for a decision; none is chosen
yet.

## Goal

Make the A* search's per-SWAP cost reflect:

```text
cost(edge, labelPair) = typedSwapCost(labelPair) x hopMultiplier(edge)
```

where `hopMultiplier` is 1 for a physical nearest-neighbour (NN) coupling edge
and 3 for a physical next-nearest-neighbour (NNN) edge (`2<>4`, `5<>7`, `8<>10`
on the current target), and `typedSwapCost` is the existing `AA=1, AB=2, BB=3`
table in `Node::typedSwapCost` (Mapping.cpp:251-257).

Relevant existing code, for reference:

- `CompilerTarget::Coupling` (`Target.h:44`) is a bare
  `std::pair<SiteId, SiteId>` — no weight field, no notion of physical distance
  beyond graph adjacency.
- `Node`'s constructor (`Mapping.cpp:207-220`) currently does:
  `if (useTypedCost) { pathCost += typedSwapCost(labels of prog0, prog1); }` —
  it has access to `swap.first`/`swap.second` (the hardware edge) but never uses
  it for cost, only for applying the swap to the layout.
- Today's `AA=1, AB=2, BB=3` values are plain hardcoded literals inside
  `typedSwapCost` — not exposed as a pass `Option`, unlike `alpha`/`lambda`/
  etc. (Noted here since it's directly relevant to design question 2 below; no
  decision recorded yet on whether to also make these three tunable.)

## Design questions — resolved

1. **CLI-exposed pass `Option` vs. C++-only constructor field?** →
   **CLI settable.** Decision: the NN/NNN edge information should be a proper
   TableGen `Option` (like `alpha`, `qubitTypeLabels`, etc.), string-encoded and
   parsed similarly to `qubitTypeLabels` (e.g. something like `"2-4,5-7,8-10"`),
   because it may not always apply to every compilation target the pass is run
   against — it needs to be something a caller can simply omit/leave empty for
   targets where it's irrelevant, the same way `qubitTypeLabels` defaults to
   empty and disables its heuristic.
2. **Fixed x3 constant vs. a general per-edge float multiplier?** →
   **Tunable but optional parameter**, mirroring `nlookahead`/`alpha`/`lambda`:
   a new `Option` (e.g. `nnn-cost-multiplier`, float, defaulting to the
   confirmed empirical value `3.0`) rather than a hardcoded literal. Explicitly
   noted: this puts the new multiplier on more consistent footing than today's
   `AA=1/AB=2/BB=3` constants, which are currently *not* tunable pass options
   even though they're conceptually the same kind of empirical prior — flagged
   for awareness, not decided as in-scope for this change.

These two answers mean the eventual pass surface likely looks like two new
sibling `Option`s next to `qubitTypeLabels`: one string (the NNN edge list) and
one float (the multiplier), both optional/defaulted so omitting them reproduces
today's behavior exactly.

## Implementation options for *where the NN/NNN classification lives*

### Option A — pass-local opt-in edge set, mirroring `qubitTypeLabels`

Add a new opt-in field alongside `qubitLabels` in `MappingPass` — e.g. a
`DenseSet<IndexPairType>` of canonicalized (min, max) hardware-edge pairs that
get the NNN multiplier, parsed once at pass setup from the new `nnn-edges`-style
`Option` string (per resolved question 1). Generalize the cost function to:

```text
baseCost   = useTypedCost ? typedSwapCost(labels) : 1.0F;
edgeMult   = nnnEdges.contains(canonicalize(swap)) ? nnnCostMultiplier : 1.0F;
pathCost  += baseCost * edgeMult;
```

Note this decouples the two heuristics: edge weighting can be used with or
without the A/B label heuristic (`baseCost` falls back to a flat `1.0` when
`qubitTypeLabels` is unset), rather than requiring both together. This needs a
small refactor of the current single `useTypedCost` boolean into two independent
flags (whether type-based cost applies, whether edge-based cost applies), since
today `pathCost` only accumulates anything at all when `useTypedCost` is set.

- **Pros:** smallest diff; no `CompilerTarget` changes; follows an
  already-reviewed pattern in this exact file; naturally CLI-settable via a
  string `Option`, matching resolved question 1.
- **Cons:** the "this edge is physically NNN" fact lives beside the pass rather
  than on the target description — if another pass/tool ever needs the same
  physical-distance fact about this hardware, it has to be redeclared there too.

### Option B — extend `CompilerTarget::Coupling` with a real weight

Turn `Coupling` into a small struct (`{SiteId, SiteId, double weight = 1.0}`) or
add a parallel weight accessor; switch `distanceBetween`'s precomputation from
BFS to Dijkstra so both `areAdjacent`-style adjacency and weighted distance are
available generically (would also let `h(n)` become weight-aware later, if that
turns out to matter — see the larger plan's open questions).

- **Pros:** the hardware-topology fact lives with the hardware description, the
  semantically "correct" home, reusable by any future pass/tool built on
  `CompilerTarget`.
- **Cons:** by far the largest diff — touches `Target.h`/`Target.cpp`
  (`Storage`, every constructor overload) and every existing caller that builds
  a plain `{a, b}` coupling list, including this test file. Sits awkwardly with
  resolved question 1 too: the weight would live on the target (not the pass
  invocation), so "may not always apply to the compilation target" would have to
  be expressed by whether *that specific target instance* declares weighted
  couplings at all, rather than by a per-pass-run `Option` the caller can freely
  omit.

### Option C — auto-derive NN/NNN from site-ID arithmetic

No new list or option at all: inside the cost function, classify
`swap.first`/`swap.second` as NNN whenever `|a - b| > 1`, since this particular
target happens to number its sites along the chain.

- **Pros:** zero new surface area, nothing to keep in sync.
- **Cons:** implicit index-arithmetic "magic" that only happens to work because
  of how *this* target numbers its sites; silently misclassifies edges (wrong
  answer, not a crash) if site numbering ever changes or the same cost logic
  gets reused for a differently-laid-out target. Also doesn't naturally support
  resolved question 1 (nothing to make CLI-settable — it's derived, not
  declared) or question 2 in isolation (would still need a separate multiplier
  `Option` bolted on). Fragile as a real feature, fine only as a one-off
  shortcut.

### Option D — explicit small edge list declared at the test's construction site (specialization of A)

Rather than inventing a generic "edge classifier," declare the 3 known NNN edges
as a named constant string next to `getRydbergIonTarget()` in the test file
(mirroring the `kNLookahead`/`kAlpha`/... constants added earlier in this same
file for the other pass options), and pass that string as the value of the new
`nnn-edges` `Option` from resolved question 1.

- **Pros:** no guessing, no invented abstraction beyond what Option A already
  needs; consistent with the "editable constants at the top of the file" pattern
  already in place here; still fully CLI-settable for any other caller, since
  it's just supplying a value to the same `Option`.
- **Cons:** doesn't generalize past hand-maintained lists — fine for one fixed
  target, more tedious if there end up being several different Rydberg-ion
  target variants to maintain lists for.

## Status

Not decided yet — presented for review. Option D (== Option A's mechanism, with
this test's specific edge list authored as a file-local constant) was the
initial leaning going into this round, and is fully compatible with both
resolved design questions (CLI-settable string `Option` for the edge list,
tunable float `Option` for the multiplier). Option B remains the more
"architecturally correct" answer if this ever needs to generalize beyond one
hand-built test target, but is a much larger change and arguably premature
before Phase 2 (automating the simulate/downgrade pipeline, see the larger plan)
exists to prove out whether the static prior is even worth the complexity.
