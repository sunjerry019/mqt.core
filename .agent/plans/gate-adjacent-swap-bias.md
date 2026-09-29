# Prefer SWAPs that immediately follow a two-qubit gate on the same qubit pair

This ExecPlan is a living document. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
be kept up to date as work proceeds.

This ExecPlan must be maintained in accordance with `.agent/PLANS.md` from the
repository root.

This plan supersedes an earlier plan
`.agent/plans/gate-adjacent-swap-statistic.md`, which covered only the
measurement half of this work. That file carries a superseded notice and must
not be implemented separately; everything in it is incorporated here as
Milestone 1.

## Purpose / Big Picture

When the compiler inserts a SWAP operation into a quantum circuit, that SWAP
must later be built either in a cheap unprotected form or in an expensive
protected form that stops a single fault from spreading into an error the code
cannot correct. On this hardware the protected form costs roughly seven and a
half times the unprotected one. Which SWAPs need protecting is decided outside
this compiler, by a slow simulation in a separate project.

There is one situation where the answer is believed to be known in advance and
for free. If a SWAP acts on exactly the same pair of qubits that a two-qubit
gate of the original circuit has just acted on, with nothing in between touching
either qubit, then those two qubits were already interacting directly a moment
earlier. The SWAP opens no path for a fault to spread that the gate had not
already opened, so the original circuit's own fault tolerance covers it and the
SWAP can stay cheap.

The compiler has no notion of this today and produces such SWAPs only by
accident. This plan makes it produce them deliberately, by telling the search
that they are cheaper than they look.

The whole plan is built around the fact that its own payoff is uncertain. It
therefore proceeds through four gates, each of which can stop the work with a
recorded finding rather than a completed feature. The first gate costs nothing
and runs entirely against data already on disk; the expensive parts happen only
if it passes.

You can see the finished work operating by routing the same circuit across many
random seeds with and without the discount and comparing how many qualifying
SWAPs appear: the average must rise, and the average total SWAP cost must not
rise with it.

## Progress

- [x] (2026-09-22) **Gate 0.** Tested the premise against existing data: all
      seven qualifying SWAPs across the two checked-in routings were left
      unprotected by the simulator. The premise holds; the finding and the
      estimated ceiling on benefit are recorded in `Outcomes & Retrospective`.
      The script is `scripts/gate_adjacent_swap_premise.py` in the sibling
      repository. The two opposite walk orders over `4.in` were also compared
      and agree on every SWAP.
- [x] (2026-09-26) **Gate 1.** Added the per-site gate tracking, the qualifying
      predicate and the statistic; see `Surprises & Discoveries` for two things
      that did not go as the plan anticipated, both resolved with the user's
      input. No routing decision changes.
- [x] (2026-09-26) Confirmed routed output is unaffected by Milestone 1: 89/89
      mapping-pass GoogleTests and 2/2 Rydberg-ion GoogleTests pass, and the
      Rydberg-ion binary was run three times in a row with identical results.
- [x] (2026-09-28) **Gate 2.** Added the multi-seed measurement loop
      (`GateAdjacentSwapBaselineDistribution` in `test_rydberg_ions.cpp`) and
      recorded the baseline distribution in `Outcomes & Retrospective`.
- [ ] **Gate 3.** Add the pass option, the activation flag and the discount.
- [ ] Add the GoogleTests described under Validation and Acceptance.
- [ ] **Gate 4.** Measure the effect across seeds at several discount values and
      decide whether to keep the option. Record the decision either way.

## Surprises & Discoveries

- Observation: only one of the two possible orderings can ever occur, which
  greatly simplifies the implementation. A SWAP cannot be immediately followed
  by a two-qubit gate on the same site pair. If after a SWAP on sites a and b
  the gate's two qubits occupy a and b, then before the SWAP they occupied b and
  a, the same unordered pair, so they were already adjacent and the gate was
  already executable; the search returns an empty SWAP sequence in that case
  without emitting anything. Evidence: scanning four routed Bacon-Shor circuits
  found zero occurrences of a SWAP followed by a gate on the same pair, and
  five, two, two and two occurrences of the reverse ordering. Only the
  gate-then-SWAP ordering therefore needs to be handled, and only for the first
  SWAP of each search, because no gates execute during a search.

- Observation: a first attempt at that scan produced zero everywhere because it
  searched forward from each SWAP for a following gate, which as the previous
  point explains can never find anything. The correct scan walks forward from
  every two-qubit gate looking for a following SWAP, or equivalently backward
  from every SWAP. Recording this because the same mistake is easy to repeat
  when writing the Gate 0 script.

- Observation: the two walk orders over `4.in` produce *identical* verdicts on
  all seventy-six SWAPs, so on this routing the greedy walk's order-dependence
  is zero and its verdicts are not merely an upper bound. The two walks are
  recorded in `scripts/test_cases/4.btf.out` (back-to-front) and
  `scripts/test_cases/4.ftb.out` (front-to-back), not in the checkpoint files,
  which the orientation section wrongly identified as the two directions; the
  two checkpoint JSONs are byte-identical to each other and agree with both
  walks. The walk direction is confirmed from the logs themselves rather than
  their names: the back-to-front log prints an explicit descending swap index,
  and the front-to-back log, which prints no index, matches its own resulting
  circuit only when its progress lines are read in forward order. Their per-swap
  progress lines differ, which is purely the visiting order; their final
  circuits are identical line for line apart from the output path.

- Observation: the verdicts are more reliably read from the resulting
  `CompiledCircuit` repr at the end of a `--mode greedy` log than from either
  the progress lines or the checkpoint. The repr spells out every SWAP in
  circuit order as a plain `swap` or a three-site `swap_ft`, which is
  unambiguous even for the front-to-back log whose progress lines carry no swap
  index, and it can be cross-checked against the routing's own SWAP site pairs.
  The Gate 0 script does that cross-check before trusting any verdict source.

- Observation: the discount is silently inert unless one of the two existing
  cost heuristics is already switched on, and the implementation must handle
  this explicitly rather than assume otherwise. Evidence: in the `Node`
  constructor in `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp` at
  approximately line 219, the whole cost accumulation is wrapped in
  `if (useTypedCost || useEdgeCost) { ... }`, so with neither heuristic active
  `pathCost` is never incremented; and `g()` at approximately line 259 returns
  `alpha * ((useTypedCost || useEdgeCost) ? pathCost : static_cast<float>(depth))`,
  so with neither active the search ranks purely by how many SWAPs deep a node
  is and ignores `pathCost` entirely. A discount written inside that guard
  therefore does nothing at all in the default configuration, and a test that
  enables only the discount will fail for a reason that is not obvious from its
  symptoms.

- Observation: the expected size of the benefit is small unless the discount
  moves the count a long way. Arithmetic: a routing contains roughly eighty
  SWAPs of which two to five qualify today, and roughly half of all SWAPs end up
  protected. If the discount doubles the qualifying count to about six, that is
  three extra SWAPs guaranteed unprotected, of which about one and a half would
  otherwise have been protected, each saving about six and a half cost units
  where a bare nearest-neighbour SWAP costs one. Against a total routing cost in
  the high hundreds that is low single-digit percent. The change is worth
  pursuing only if the count can be pushed substantially higher than double,
  which is exactly what Gate 4 measures. Anyone running this should know the
  shape of the answer before seeing it.

- Observation: the pattern this plan biases towards is the structural backbone
  of an expert compilation rather than an incidental feature of router output.
  Evidence: `scripts/manual_compilations.py` in the sibling repository, added
  2026-09-22, spells out two hand-written Bacon-Shor memory rounds gate by gate
  in physical-site terms. Applying the Milestone 1 predicate to their gate lists
  gives 28 qualifying SWAPs out of 90 in `MANUAL_OPTIMAL` and 15 out of 77 in
  `MANUAL_KATRIN`, against 5 out of 76 in `4.in` and 2 out of 84 in `5.in`. That
  is a qualifying rate of 31 and 19 percent against 7 and 2 percent. The reason
  the density is so much higher is visible in the circuits themselves: syndrome
  extraction walks an ancilla along the chain, and the atomic step of that walk
  is "read this data qubit, then step past it", which is a two-qubit gate
  followed immediately by a SWAP on exactly that pair. The opportunity is
  therefore inherent in the circuit being routed rather than rare, and the
  router does not take it because nothing in its cost model tells it to. When
  counting these, exclude the docstring of `_add_gates_optimal`, which lists the
  available gate calls in the same `circuit.swap(q[a], q[b])` form as real gates
  and inflates a naive text scan by two bare SWAPs, one protected SWAP and
  several gates.

- Observation: the premise now rests on fifty instances from two independent
  sources rather than seven from one. Every one of the 43 qualifying SWAPs in
  the two hand-written circuits is written as a plain `swap` and none as a
  `swap_ft`, which is the same verdict the simulator gave on all seven
  qualifying SWAPs in the routed circuits. The two sources are genuinely
  independent: the routed verdicts come from the greedy downgrade search, the
  hand-written ones from a person reasoning about the code directly.

- Observation: among SWAPs that do *not* qualify, the fraction needing
  protection is close to 0.375 in three of the four circuits regardless of who
  produced them --- 27 of 71 in `4.in`, 31 of 82 in `5.in` and 23 of 62 in
  `MANUAL_OPTIMAL`, which is 0.380, 0.378 and 0.371. `MANUAL_KATRIN` is the
  outlier at 34 of 62, or 0.548. Taking 0.375 as the rate, a SWAP that does not
  qualify costs an expected 0.375 x 7.5 + 0.625 x 1 = 3.44 units while a
  qualifying one costs 1, so moving a single SWAP into the qualifying set is
  worth about 2.44 units, or roughly seventy percent of that SWAP's expected
  cost. This is the marginal number Gate 4 should be read against, and it is far
  more useful than the doubling assumption the original ceiling calculation
  used.

- Observation: the reverse ordering is absent from the hand-written circuits
  too. Scanning both manual compilations for a SWAP immediately followed by a
  two-qubit gate on the same pair found zero occurrences, confirming on circuits
  no router produced what the first entry in this section argued from the
  search's early-exit. The argument and the evidence now agree from two
  directions.

- Observation: both hand-written circuits contain native three-qubit gates ---
  three `ccz` and three `ccx` each. Four of the six act on sites 5, 6 and 7,
  which is a triangle of the target, and two act on the fault-tolerant-swap
  ancilla site, which carries no couplings and which the file itself marks
  `OFF-MAP`. Milestone 1 already requires the any-arity tracking vector to be
  updated for three-qubit gates; this is the evidence that such gates are not
  hypothetical and that the requirement matters. It also settles the first open
  question in the sibling repository's `.agent/plans/circuit-fidelity-scorer.md`
  for the circuits that plan has to score, though not for router output.

- Observation: `generateLayout` runs up to `ntrials` trials concurrently via
  `parallelForEach` (default `ntrials` is 4; the Rydberg-ion test overrides it
  to 1). The plan's Milestone 1 text, "add two vectors to the state the routing
  loop carries", is ambiguous between a `MappingPass` member (like
  `qubitLabels`/`nnnEdgeSet`, which are safe because they are read-only after
  being parsed once) and per-trial state. Since the two new tracking vectors
  must be mutated inside `advance()`, which runs inside concurrently-executing
  trials, a `MappingPass` member would be a real data race, exactly the scenario
  the Milestone 1 acceptance criteria's required `ntrials > 1` test would
  exercise. Implemented as fields of `RoutingBundle` instead (mirroring how
  `Statistics` is already scoped per trial), reset at the start of every
  `route()` call — which also naturally resets them at each nested-region
  `dispatch()` boundary and between `generateLayout`'s forward/backward passes,
  satisfying "reset for each routing pass" without extra plumbing.

- Observation: `advance()` is built on the shared `walkProgramGraph` utility
  (`mlir/include/mlir/Dialect/QCO/Utils/Drivers.h`), whose own doc comment says
  it "traverses the def-use chain of each qubit until a multi-qubit gate
  (including barriers) is found." Concretely, single-qubit gates, `qco.reset`,
  and `qco.measure` are all classified as arity one and silently walked past —
  they never appear in the `ready`/`released` sets `advance()`'s callback sees.
  A first implementation of Milestone 1 therefore could not detect an
  intervening single-qubit gate at all (confirmed empirically: inserting an `H`
  between a qualifying gate and its SWAP left the statistic at 1, not 0). Fixed
  by adding an optional `skipped` callback to `walkProgramGraph`, invoked for
  exactly the arity-one operations it walks past (never for the purely
  structural ops like `qco.alloc`/`qco.sink`/`qco.yield`, which do not touch a
  qubit in any physical sense), defaulting to nothing so every other caller
  (including `getWindow`) is unaffected. `qco.reset`/`qco.measure` disqualify a
  pair the same way a single-qubit gate does, at the user's direction. A
  dedicated toggle, `MappingPass::K_TRACK_ARITY_ONE_OPS_FOR_ GATE_ADJACENCY` (a
  `static constexpr bool`, default `true`), turns off only this specific piece
  of tracking — flipping it to `false` reverts to disqualifying a pair solely
  via a second multi-qubit gate or a barrier — without touching anything else,
  per the user's explicit request for an easy off-switch given how new and
  untested this mechanism is.

- Observation: with that fix in place, a SWAP that reuses a qubit's site as
  workspace after that qubit has finished its own last gate is *reliably*
  disqualified, because the router measures and sinks a "finished" qubit
  immediately (as part of `advance()`'s ordinary eager wire-draining), and the
  SWAP's operand genuinely flows through that measurement's result — a real
  data-dependency, not a scheduling artifact. This is not a bug: the physical
  qubit at that site has been measured, so any fault-path argument resting on
  "this qubit was already coupled to its swap partner via the original gate" is
  void. But it means every minimal hand-built test circuit tried (the
  pre-existing triangle scenario already used by
  `StatefulSwapLabelsChangeRoutingChoice`/`StatefulSwapLabelsPreferCheaper TypedSwap`,
  and two different "ancilla steps past a data qubit" variants of four and more
  qubits) exhibited this and could not be made to produce a small,
  hand-derivable *qualifying* SWAP through the full pass: the shared qubit
  common to the two gates that let a third gate execute always turned out to
  have nothing left to do, and so was always measured first. Confirmed this is
  not specific to the new circuits: re-running the pre-existing triangle
  scenario (`qubitTypeLabels="BAA"`, seed 1) through the fixed implementation
  now reports 0 qualifying SWAPs, where before this fix (with arity-one ops
  invisible) it reported 1. `MappingPass` offers no way to pin an initial layout
  directly to sidestep `generateLayout`'s randomization (and a target without
  explicit topology is all-to-all, so no SWAP is ever needed there either), so
  this could not be worked around by construction within the time spent trying.
  Whoever runs Milestone 2's seed loop on the real Rydberg-ion/Bacon-Shor
  circuit should watch for whether this pattern (measurement immediately
  following a qubit's last gate) is common there too, since if so it would pull
  the measured qualifying-SWAP count below what Gate 0's script — which scans a
  pre-serialized gate list and has no concept of measurement or of
  independent-wire scheduling at all — found on `4.in`/`5.in`. The two
  instruments are not measuring quite the same thing.

- Decision: at the user's direction, replace the Milestone 1 acceptance test
  that traces a hand-built circuit through the full pass with a direct unit test
  of `isGateAdjacentSwap` instead, given the difficulty above. Rationale: the
  predicate itself (a four-way pointer comparison) is the part Milestone 1
  actually adds and the part worth asserting on directly; it needs no routing
  decision at all, only `Operation*` values used as opaque identity tokens. To
  make this possible, `isGateAdjacentSwap` was pulled out of `MappingPass` into
  a free function declared in the public `Mapping.h`, alongside
  `createMappingPass`. Date/Author: 2026-09-26, this plan, at the user's
  direction after reviewing the difficulty recorded above.

## Decision Log

- Decision: merge the measurement and the behaviour change into one plan with
  internal gates, rather than keeping them as two plans. Rationale: they share
  all of their machinery, so splitting them duplicated the orientation material
  and made the sequencing look like a technical dependency when it is a
  methodological one. The gates preserve the property that mattered — that the
  baseline is measured with the same instrument as the improvement, before the
  improvement exists. Date/Author: 2026-09-21, this plan, at the user's
  direction.

- Decision: make a check against existing simulator output the first gate,
  before any code is written. Rationale: the entire plan rests on the premise
  that a gate-adjacent SWAP is safe to leave unprotected. That premise is
  testable for free against routings and verdicts already on disk, and if it is
  false the rest of the plan is worthless. Spending half an hour to find that
  out first is obviously right. Date/Author: 2026-09-21, this plan.

- Decision: implement the preference as a cost discount inside the search rather
  than as a peephole rewrite applied after routing. Rationale: the goal is to
  change which placements the router chooses, and a rewrite applied afterwards
  cannot do that; it can only clean up placements that were already chosen for
  other reasons. Date/Author: 2026-09-18, this plan.

- Decision: apply the discount only to the first SWAP of each search invocation.
  Rationale: the qualifying condition refers to a gate that has already
  executed, and no gates execute during a search, so no SWAP after the first can
  qualify; restricting to the first also keeps the cost a function of the search
  node alone, which the search's duplicate-state pruning requires. Date/Author:
  2026-09-18, this plan.

- Decision: define "nothing in between" to include single-qubit gates, so that a
  Hadamard on either qubit between the gate and the SWAP disqualifies the pair.
  Rationale: this is the conservative reading, it matches the definition the
  hardware owner chose, and it matches the measurement quoted above; relaxing it
  later is a safe direction whereas tightening it is not. Date/Author:
  2026-09-18, this plan, at the user's explicit direction.

- Decision: express the option as a multiplicative discount rather than a
  boolean. Rationale: the correct discount depends on which cost model is active
  and on hardware constants that are still being measured, so a float lets the
  user calibrate it without another code change, and a value of one disables the
  feature entirely. Date/Author: 2026-09-18, this plan.

- Decision: measure with the layout search limited to a single trial until
  `.agent/plans/cost-weighted-trial-selection.md` has landed. Rationale: with
  more than one trial the search minimises the discounted cost but the selection
  among trials ranks candidates by raw SWAP count, so the selection step can
  discard precisely the candidate the discount produced, masking the effect
  being measured. The existing Rydberg-ion test already uses one trial, so this
  costs nothing. Date/Author: 2026-09-21, this plan.

- Decision: use a seed loop inside the existing GoogleTest as the measurement
  instrument, rather than waiting for the command-line tool in
  `.agent/plans/rydberg-ion-routing-tool.md`. Rationale: the measurement needs
  many seeds in one process, which a loop inside one test body provides in a few
  hours, whereas the tool is a larger extraction; the tool remains the better
  long-term instrument and this plan does not compete with it. Date/Author:
  2026-09-21, this plan, at the user's direction to prioritise this work.

- Decision: abandoning this plan after any gate is an acceptable outcome.
  Rationale: the payoff is genuinely uncertain and the change adds a branch to
  the search's hot path, so if the evidence does not support it the honest
  response is to record that and stop. Whoever reaches that point must write the
  finding into `Outcomes & Retrospective` rather than deleting the plan.
  Date/Author: 2026-09-18, this plan.

- Decision: keep the algebraic gate-and-SWAP fusion out of this plan and record
  it as a separate follow-up that reuses this plan's predicate. Rationale: a
  qualifying SWAP is cheap for two independent reasons and only one of them is a
  routing decision. The first, which is this plan, is that it may stay
  unprotected, worth about 6.5 units. The second is an exact circuit identity: a
  two-qubit gate followed by a SWAP on the same pair is two entangling gates
  rather than four, because SWAP is `C(x,y) C(y,x) C(x,y)` and the leading
  factor annihilates the gate. The identity holds for CZ as well as for
  controlled-NOT, since `SWAP . CZ` equals `(H (x) I) . SWAP . CNOT . (I (x) H)`
  and is therefore locally equivalent to `SWAP . CNOT`, whose
  two-entangling-gate realisation is the same one. That saving is about two
  thirds of a unit per instance and, unlike the first, needs no fault-tolerance
  premise at all: it is arithmetic. But realising it is a peephole rewrite
  applied after routing, in a different pass, and folding it in here would break
  the scope this plan is deliberately narrow about. The connection to record is
  that the Milestone 1 statistic counts fusion opportunities and protection
  opportunities with the same predicate, so this plan's instrumentation is also
  the measurement instrument for that follow-up, and the discount makes both
  payoffs larger together. Date/Author: 2026-09-22, this plan, prompted by the
  arrival of `scripts/manual_compilations.py`.

## Outcomes & Retrospective

**Gate 0, 2026-09-22: the premise holds; proceed.** Measured by
`scripts/gate_adjacent_swap_premise.py` in the sibling repository, run as
`uv run python scripts/gate_adjacent_swap_premise.py`, which reads the routings
and checkpoints only.

`scripts/test_cases/4.in` contains 76 SWAPs, of which the simulator left 27
protected and downgraded 49 to bare, a protected fraction of 0.355. Five SWAPs
qualify: swap indices 2, 10, 16, 40 and 67, on site pairs (2,4), (4,5), (8,10),
(1,2) and (8,9), each immediately preceded by a `cx` on exactly that pair. All
five were left bare.

The two walk orders agree completely. Checked against the back-to-front walk in
`scripts/test_cases/4.btf.out`, the front-to-back walk in
`scripts/test_cases/4.ftb.out` and both checkpoint files, all four verdict
sources give the same decision for every one of the 76 SWAPs; no swap index
differs anywhere. The order-dependence of the greedy walk is therefore zero on
this routing, which is a stronger result than the plan anticipated: on `4.in`
the verdicts are not merely an upper bound on how many SWAPs need protection but
appear to be the answer. One routing is not proof that this holds generally, and
`5.in` has only one walk, so the caveat stays live for other inputs.

`scripts/test_cases/5.in` contains 84 SWAPs, of which 31 were left protected and
53 downgraded, a protected fraction of 0.369. Two SWAPs qualify: indices 40 and
66, on pairs (1,2) and (4,5). Both were left bare.

No qualifying SWAP was left protected anywhere, so nothing needs the
greedy-ordering explanation the plan held in reserve. The measured counts of
five and two match the four-circuit scan quoted in `Surprises & Discoveries`,
which is a consistency check on the scan rather than new information.

Ceiling on benefit, computed as the plan prescribes: assume the discount doubles
the qualifying count, multiply the increase by the measured protected fraction,
and multiply by the protected-minus-bare cost difference of 6.5 units. For
`4.in` that is 5 x 0.355 x 6.5 = 11.5 units against a total SWAP cost of 251.5,
or 4.6 percent. For `5.in` it is 2 x 0.369 x 6.5 = 4.8 units against 285.5, or
1.7 percent. Total SWAP cost here is the measured verdicts priced at one unit
per bare SWAP and seven and a half per protected one. These figures are the
ceiling on a doubling, not an expectation: the real effect is whatever fraction
of that doubling the discount actually achieves, and Gate 4 measures it. Low
single-digit percent is the shape of the answer to expect, as the fourth entry
in `Surprises & Discoveries` already predicted.

The script reports each routing under two readings of "nothing in between",
differing in whether a preceding routing SWAP on exactly the same site pair
blocks qualification or is transparent. The Milestone 1 predicate as specified
implements the transparent reading, because it tracks only gates executed in
`advance()` and inserted SWAPs never update it. On this data the two readings
agree exactly, in every routing, so the choice is unobservable here and
Milestone 1 need not revisit it. It may separate the two once the discount
starts producing same-pair SWAP chains, which is worth rechecking at Gate 4.

One caveat carried forward, weakened by the agreement above: the verdicts come
from a greedy, single-pass search, so in general they are an upper bound on how
many SWAPs genuinely require protection rather than an exact count. On `4.in`
the two opposite walk orders agree exactly, so on that routing the bound is
tight; whether that survives to other routings is untested, `5.in` having been
walked only once.

**Gate 0 addendum, 2026-09-22: the ceiling is several times larger than the
doubling assumption implied.** Two hand-compiled Bacon-Shor memory rounds
arrived as `scripts/manual_compilations.py` in the sibling repository, and
applying the Milestone 1 predicate to them gives qualifying rates of 31 percent
(`MANUAL_OPTIMAL`, 28 of 90 SWAPs) and 19 percent (`MANUAL_KATRIN`, 15 of 77)
against the router's 7 and 2 percent. The original ceiling assumed the discount
doubles the qualifying count, which was a guess made with no evidence about what
rate is attainable; there is now evidence, and it is between three and five
times the router's rate.

Recomputed with the expert rate in place of the doubling, and using the marginal
figure of 2.44 units per SWAP moved into the qualifying set derived in
`Surprises & Discoveries`: raising `4.in` from 5 qualifying SWAPs to 31 percent
of 76, or about 24, is 19 SWAPs at 2.44 units, or 46 units against a total SWAP
cost of 251.5 --- 18 percent. The same calculation on `5.in` gives 24 SWAPs at
2.44 units, or 59 units against 285.5 --- 21 percent. Those two totals are the
flat metric, so the calculation should be redone with each SWAP priced by the
connection it crosses. Doing so: a SWAP moved into the qualifying set saves 2.46
units on a nearest-neighbour connection and 6.42 on a diagonal, and weighting by
each circuit's own mix gives 72 units against 503.0 for `4.in`, or 14 percent,
and 62 against 307.0 for `5.in`, or 20 percent. The conclusion is insensitive to
the metric even though the totals are not. Either way the expected shape of the
answer moves from low single-digit percent to between a seventh and a fifth of
the routing cost, and the recommendation to proceed past Gate 0 is much better
supported than it was.

Three cautions attach to that number and none of them should be dropped when it
is quoted. First, the hand-written circuits are not an achievability target for
the router and the file says so in terms: they use moves that are out of spec,
add resets the router would not, and exploit knowledge of which qubits are gauge
qubits, and its author writes that reproducing them is explicitly not the aim.
They establish that the opportunity exists in the circuit, not that a cost
discount can capture it. Second, the marginal figure rests on a non-qualifying
protection rate of 0.375, which three of the four circuits agree on closely but
`MANUAL_KATRIN` contradicts at 0.548; if the true rate is nearer Katrin's the
marginal value per SWAP rises rather than falls, so this caution runs in the
favourable direction, but it means the rate is not yet established. Third, the
ceiling remains a ceiling. Gate 4 measures what fraction of it the discount
actually reaches, and a result far below it is still a legitimate outcome to
record.

Two cautions about comparing costs against these circuits, as opposed to this
plan's own arithmetic. The first is the metric. Priced flat, at one unit per
bare SWAP and seven and a half per protected one, `MANUAL_OPTIMAL` scores 239.5
and `MANUAL_KATRIN` 298, against 251.5 for `4.in` and 285.5 for `5.in`, which
reads as the router being within five percent of the expert compilation. That
reading is an artefact of the flat metric. Pricing each SWAP by the connection
it crosses gives 240.9, 300.0, 503.0 and 307.0, because the two hand-compiled
circuits never swap across a diagonal at all while `4.in` does so 27 times in
76. On that circuit the router is about 2.1 times more expensive than
`MANUAL_OPTIMAL`, not five percent. Use the edge-aware figures for any
comparison across routings with different edge mixes. The second caution is
provenance, and it survives the correction above: the routed circuits'
protected-or-bare verdicts come from the simulator's greedy downgrade search,
whereas the hand-written circuits' `swap` and `swap_ft` choices are their
author's, made by hand. The apples-to-apples comparison is to run the same
greedy downgrade over the manual circuits and price the result, and it is worth
doing --- it either lowers the manual circuits' cost, which sharpens the target,
or it reports that some of their bare SWAPs need protection, which would be a
finding about the reference itself. That check belongs to the sibling
repository's evaluation work rather than to this plan.

On completion of Gate 2, record the distribution of the qualifying-SWAP count
and of the total SWAP count across the measured seeds: mean, spread and range,
not a single number.

**Gate 2, 2026-09-28: baseline distribution measured.**
`GateAdjacentSwapBaselineDistribution` in `test_rydberg_ions.cpp` routes the
same twelve-qubit Bacon-Shor circuit used by the two existing Rydberg-ion tests
(`qubitTypeLabels` nine `B` then three `A`, `nnnEdges` `2-4,5-7,8-10`, so both
existing cost heuristics are active, matching
`MapBaconShorCodeOnRydbergIonTarget`) once per seed, for seeds 0 through 29,
with `ntrials` fixed at one per the `Decision Log`. It asserts nothing about the
resulting numbers, only that each of the thirty routing attempts succeeds, and
prints the mean, minimum and maximum of both `num-inserted-swaps` and
`num-gate-adjacent-swaps` across the run.

Because the pass is itself non-deterministic between process launches (the
orientation section's `ReadyMap`-keyed-on-heap-address observation), the
thirty-seed loop was itself run six times to see the spread between runs, not
just within one. `num-inserted-swaps` means ranged from 92.43 to 95.80 across
the six runs (one low outlier at 92.43, the other five within 0.5 of each
other), with an overall minimum of 72 and an overall maximum of 120 across all
seeds and all runs. `num-gate-adjacent-swaps` means ranged from 7.37 to 8.33,
with an overall minimum of 1 and an overall maximum of 21. Taking a
representative run (mean 95.63 total, mean 8.20 qualifying), the qualifying
fraction is about 8.6 percent, which lines up with the 7 and 2 percent Gate 0
measured on `4.in`/`5.in` and corroborates that those two statically-routed
circuits are representative of what this pass produces on a comparable
Bacon-Shor circuit, rather than an artifact of those two specific inputs.

The full thirty-seed loop completed in 44 to 86 milliseconds end to end across
the runs observed (including MLIR context and module construction for all thirty
seeds, not routing alone), well inside the roughly one-second budget the plan
estimated from a per-routing figure of about 36 milliseconds; the actual
per-routing cost on this circuit is well under that figure, so the test needs no
further guard against slowing the ordinary test run.

On completion of Gate 4, record the same distributions at each discount value
tried, and state plainly whether the count rose, whether total cost fell, and
whether the option should be kept.

## Context and Orientation

Everything below is a fact about the repositories as they stand. Verify each
claim before relying on it.

Terms, in plain language. The *mapping pass* inserts SWAP operations into a
quantum circuit so that every multi-qubit gate acts on hardware positions the
machine directly connects. It is declared in
`mlir/include/mlir/Dialect/QCO/Transforms/Passes.td` as `def MappingPass`,
command-line name `place-and-route`, and implemented as the single `MappingPass`
struct in `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`. A *site* is a
hardware qubit position, numbered from zero. A *program qubit* is a qubit of the
user's circuit; which site holds which program qubit changes as SWAPs execute,
and that assignment is the *layout*. A *fault-tolerant SWAP*, also called
protected, is the expensive realisation; a *bare SWAP* is the cheap one. The
pass never decides which is used; it only weighs how likely the expensive form
is.

This repository is `/Users/yudong/Documents/projects/mqt.core`. The sibling
project holding the simulator and its recorded verdicts is
`/Users/yudong/Documents/projects/qec-rydberg-ions`, whose Python is run through
`uv run`.

The routing loop works as follows. `advance()`, at approximately line 1438,
executes every gate whose qubits already sit on directly connected sites, and
stops when no more can be executed. `getWindow()`, at approximately line 1343,
collects the gates that are blocked, in layers. `search()`, at approximately
line 1091, runs an A\* search for a sequence of SWAPs that makes the first layer
executable. `insertSWAPs`, at approximately line 1394, writes those SWAPs into
the circuit and records `stats.nswaps += swaps.size();` at approximately line
1431. The loop then returns to `advance()`. Crucially, no gates execute between
the start and the end of one `search()` call, so every SWAP a single search
emits sees the same set of already-executed gates.

The search expands a node by trying every coupling edge incident to a qubit in
the front layer, at approximately lines 1155 to 1170. It early-exits before
expanding anything if the front layer is already executable, at approximately
lines 1104 to 1109; this is the fact that makes the SWAP-then-gate ordering
impossible.

Each search node accumulates a `pathCost` in its constructor at approximately
lines 211 to 232. The accumulation today is, as indented source:

    if (useTypedCost || useEdgeCost) {
      float base = 1.0F;
      if (useTypedCost) {
        const auto [prog0, prog1] =
            layout.getProgramIndices(swap.first, swap.second);
        base = typedSwapCost(qubitLabels[prog0], qubitLabels[prog1]);
      }
      const float edgeMultiplier =
          (useEdgeCost && nnnEdges.contains(swap)) ? nnnCostMultiplier : 1.0F;
      pathCost += base * edgeMultiplier;
    }

and the ranking function `g()`, at approximately line 259, is:

    return alpha * ((useTypedCost || useEdgeCost)
                        ? pathCost
                        : static_cast<float>(depth));

Note both guards carefully; they are the reason Milestone 3 must introduce a
third activation flag rather than simply multiplying inside the existing
expression.

A node's `depth` field records how many SWAPs lie between it and the root, so
the first SWAP of a search is the one on a node of depth one. The search prunes
a node if it has already seen the same layout at an equal or lower depth, at
approximately lines 1113 and 1126 to 1136, using a map keyed on the layout. This
is why a cost term must be a function of the node itself and not of how the node
was reached: a term depending on the path would make two nodes with the same
layout genuinely different, and the pruning would discard the cheaper one. A
discount applied only at depth one satisfies this, because depth is part of the
node.

`struct Statistics`, at approximately line 160, currently holds one field,
`size_t nswaps{0};`. The pass declares a statistic `numSwaps` with command-line
name `num-inserted-swaps` in the tablegen file and accumulates it in
`runOnOperation` at approximately line 476.

Two existing pass options establish the pattern to follow, both opt-in and both
defaulting to off. `qubitTypeLabels` takes a string with one character per
program qubit, `A` for auxiliary and `B` for data, and makes a SWAP cost one,
two or three according to the pair of roles. `nnnEdges` takes a comma-separated
list of site pairs such as `2-4,5-7,8-10` and multiplies the cost of SWAPs
crossing them by `nnnCostMultiplier`. Their parsers, `parseQubitLabels` and
`parseNnnEdges`, are at approximately lines 489 and 522 and are called from
`runOnOperation` at approximately lines 411 to 432.

The existing test that exercises this configuration is
`mlir/unittests/Dialect/QCO/Transforms/RydbergIons/test_rydberg_ions.cpp`, whose
target has twelve sites and which invokes the pass with `nlookahead` five,
`niterations` one, `ntrials` one, `seed` forty-two, `qubitTypeLabels` set to
nine `B` characters followed by three `A` characters, and `nnnEdges` set to
`2-4,5-7,8-10`. Both cost heuristics are therefore active there, which is why
the activation problem above does not show up in that test.

The pass does not produce identical output across repeated runs of the same
binary with the same inputs and the same seed. The cause is
`mlir/include/mlir/Dialect/QCO/Utils/Drivers.h` line 37, which declares
`using ReadyMap = llvm::SmallDenseMap<Operation*, SmallVector<size_t>, 8>;`, a
map keyed on heap addresses, iterated during routing at `Mapping.cpp` line 1354.
Five consecutive runs of the Rydberg-ion test with identical inputs produced 83,
85, 86, 86 and 93 inserted SWAPs. Every measurement in this plan is therefore a
measurement of a distribution, and no single pair of runs proves anything.

For Gate 0, the data already on disk in the sibling repository is as follows.
Two routed circuits are checked in as `scripts/test_cases/4.in` and
`scripts/test_cases/5.in`. A routing report is a text file whose parser is
`parse_mapper_output` in `src/mapper_output.py`; it returns an object with a
`gates` field, a list whose entries have a `kind` among `reset`, `h`, `x`, `cx`,
`cz`, `ccx`, `ccz` and `swap`, and a `qubits` tuple of site indices, in circuit
order. The simulator's verdicts are checked in as
`scripts/output/4.in.greedy_checkpoint.json`,
`scripts/output/4.in.greedy_checkpoint.btf.json` and
`scripts/output/5.in.greedy_checkpoint.json`. Each is a single-line JSON object
with keys `num_swaps`, `next_index` and `ft_status`, the last being a list of
booleans, one per SWAP **in circuit order**, where true means the SWAP remained
protected and false means it was downgraded to bare. The two checkpoints for
`4.in` are byte-identical and are *not* the two walk directions. The two
directions are the full run logs `scripts/test_cases/4.btf.out`, back-to-front,
and `scripts/test_cases/4.ftb.out`, front-to-back; each ends with a
`CompiledCircuit` repr naming every SWAP as a plain `swap` or a `swap_ft` in
circuit order, which is where their verdicts should be read from. Comparing
those two measures directly how order-dependent the verdicts are; Gate 0 did so
and found no difference at all.

The hardware facts this plan relies on, supplied by the hardware owner and
re-confirmed by them on 2026-09-23. The native two-qubit entangling gate is CZ.
A CZ has fidelity 0.9975 on a nearest-neighbour connection and 0.9861 on a
next-nearest-neighbour one, so their infidelities are 0.0025 and 0.0139. A bare
SWAP is three CZ gates on the connection it crosses. A protected SWAP between
two nearest neighbours is nine CZ, arranged as three bare SWAPs through an
ancilla --- and the third of those three runs on a next-nearest-neighbour
connection, not a nearest-neighbour one, because the ancilla sits at the far
vertex of one of the target's triangles and each triangle has two
nearest-neighbour edges and one next-nearest-neighbour diagonal. A protected
SWAP between two next-nearest neighbours is three of those, twenty-seven CZ.

The unit of cost throughout this plan is the expected error of one bare
nearest-neighbour SWAP, which is 3 x 0.0025 = 0.0075. Summing infidelities,
which is valid while the individual rates are small, the four cases cost 1.00
for a bare nearest-neighbour SWAP, 5.56 for a bare next-nearest-neighbour one,
7.56 for a protected nearest-neighbour one --- the figure this plan rounds to
seven and a half --- and 22.68 for a protected next-nearest-neighbour one. The
next-nearest-neighbour leg is load-bearing in that 7.56: nine CZ priced entirely
at nearest-neighbour fidelity would give 3.00, and an earlier estimate did
exactly that. The derivation is reproduced in
`cda-lab-notes/qec-rydberg-ions/2026-09-16_paper_framing.md` and written out as
prose in `.agent/plans/fidelity-weighted-swap-cost.md`.

A warning about the one-and-seven-and-a-half shorthand this plan uses for quick
arithmetic. It prices every SWAP as though it crossed a nearest-neighbour
connection, which is false for a routing that uses the diagonals, and the error
is not uniform across circuits: `4.in` puts 27 of its 76 SWAPs on diagonals,
`5.in` 2 of 84, and the two hand-compiled circuits none at all. Pricing each
SWAP by the connection it actually crosses leaves `5.in` and both manual
circuits within one to eight percent of their flat figures but doubles `4.in`,
from 251.5 to 503.0. The shorthand is adequate for comparing two routings of
similar edge mix and wrong for comparing routings that differ in it; state which
is in use.

## Plan of Work

### Milestone 0 — test the premise against existing verdicts

Write a throwaway analysis script; it belongs in the sibling repository and is
to be manually checked in by the user. Its output must be recorded in this plan.

For each of the two checked-in routings, parse the report and walk its gate list
in order, keeping a counter of how many SWAPs have been seen so far, which is
the index into the verdict list. For each SWAP on sites a and b, scan backwards
from it through the gate list for the first gate that touches either a or b. The
SWAP qualifies if that gate is a two-qubit gate acting on exactly the pair a and
b. Scanning backwards is the correct direction; scanning forwards from the SWAP
finds nothing, for the reason recorded in `Surprises & Discoveries`.

For every qualifying SWAP, read its verdict from the checkpoint's `ft_status` at
the SWAP's index. Report how many qualified and how many of those were left
bare. Run it for `4.in` against both of its checkpoints and for `5.in` against
its one.

Interpret as follows. If every qualifying SWAP was left bare, the premise holds
and the work is worth continuing. If some were left protected, do not proceed
until you understand why: either the premise is wrong, in which case stop and
record that, or the greedy walk's ordering prevented a downgrade that was in
fact available, in which case the two `4.in` checkpoints will probably disagree
with each other and that disagreement is the evidence. A premise that fails here
cannot be rescued by anything later in this plan.

Then compute the ceiling on benefit and write it into
`Outcomes & Retrospective`: take the measured qualifying count, assume the
discount doubles it, multiply the increase by the fraction of SWAPs that are
otherwise protected, and multiply that by the difference between the protected
and bare costs. Compare the result with the total routing cost. This number sets
expectations for Gate 4 and should be computed before anybody has an emotional
stake in the outcome.

### Milestone 1 — instrumentation, with no change in behaviour

In `mlir/lib/Dialect/QCO/Transforms/Mapping/Mapping.cpp`, add two vectors to the
state the routing loop carries, each indexed by site and holding an
`Operation*`, sized to the target's site count and initialised to null. The
first records the most recent two-qubit gate to have acted on that site; the
second records the most recent gate of any arity. Place them with the routing
loop's other per-run state and make sure they are reset for each routing pass,
including the refinement passes `generateLayout` runs, so a stale entry cannot
leak between passes.

Update both in `advance()` where a gate is recorded as executed. For every site
the gate touches, set the any-arity entry to that gate; if the gate acts on
exactly two qubits, also set the two-qubit entry for both its sites. Do this for
gates of every arity including the native three-qubit gates, because a
three-qubit gate touching a site must invalidate that site's pair even though it
never establishes one.

Add a predicate taking a pair of sites. The pair qualifies when the most recent
two-qubit gate on the first site is not null, is the same operation as the most
recent two-qubit gate on the second site, and is also the most recent gate of
any arity on each of the two sites. That last condition enforces "nothing in
between", including single-qubit gates, because any later gate on either site
would have overwritten that site's any-arity entry.

Add `size_t nGateAdjacentSwaps{0};` to `struct Statistics`. In `insertSWAPs`,
evaluate the predicate on each emitted SWAP's site pair and increment the
counter. Within one call the tracking vectors do not change, because no gates
execute during a search, so evaluating per SWAP is correct; add a comment saying
so, because a later reader will suspect otherwise.

In `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`, add a statistic beside
`numSwaps` named `numGateAdjacentSwaps`, command-line name
`num-gate-adjacent-swaps`, documented as the number of inserted SWAP operations
that acted on exactly the site pair of the immediately preceding two-qubit gate
with no intervening gate on either site. Accumulate it in `runOnOperation`
beside `numSwaps`.

Nothing in this milestone may change which SWAPs are chosen. If routed output
changes, something is wrong.

### Milestone 2 — a measurement instrument and the baseline

Add a seed loop to
`mlir/unittests/Dialect/QCO/Transforms/RydbergIons/test_rydberg_ions.cpp` as a
new `TEST_F` beside the existing ones. It builds the program and runs the pass
once per seed for at least thirty seeds within the one process, collecting the
SWAP count and the qualifying count each time, and prints a summary giving the
mean, the minimum and the maximum of each. Keep `ntrials` at one, for the reason
in the `Decision Log`.

Do not assert on the numbers. This test exists to print a distribution, and any
threshold written into it now would be a guess. Guard it so that it does not
slow the ordinary test run unreasonably; at roughly thirty-six milliseconds per
routing, thirty seeds is about a second, which is acceptable, but check rather
than assume.

Run it and record the baseline distribution in `Outcomes & Retrospective`. That
distribution, not the four-circuit scan quoted in `Surprises & Discoveries`, is
the number Gate 4 compares against.

### Milestone 3 — the discount

In `mlir/include/mlir/Dialect/QCO/Transforms/Passes.td`, add one option to the
`let options` list of `def MappingPass`, named `gateAdjacentSwapDiscount`,
command-line name `gate-adjacent-swap-discount`, of type `float`, default
`1.0F`, documented as the factor by which a SWAP's cost is multiplied when it
acts on exactly the site pair a two-qubit gate has just acted on with nothing in
between touching either site; a value of one, the default, disables the feature.
Extend the pass description with a prose paragraph in the style of the two
existing heuristic paragraphs.

In `Mapping.cpp`, compute once per `search()` call, before the search begins,
the set of site pairs satisfying the Milestone 1 predicate. There is at most one
qualifying partner per site but several distinct pairs may qualify at once, so
build a small set rather than a single pair, and pass it to the node constructor
alongside the existing `nnnEdgeSet` and `qubitLabels`.

Introduce a third activation flag, for example `useGateAdjacentCost`, set when
`gateAdjacentSwapDiscount` differs from one, and include it in **both** guards
quoted in the orientation section: the `if (useTypedCost || useEdgeCost)` around
the cost accumulation, and the conditional inside `g()`. Without this the
discount has no effect whenever neither existing heuristic is enabled. State the
consequence in the option's documentation: with only the discount enabled, every
SWAP costs one except qualifying ones which cost the discount, so the search
minimises a weighted count rather than a plain depth — the intended meaning, but
a different objective from the default that a reader will not guess.

In the node constructor, after computing `base` and `edgeMultiplier` as today,
multiply by `gateAdjacentSwapDiscount` when the node's depth is one and the
node's SWAP is in the qualifying set. Guard it so that with the option at one
the arithmetic is unchanged. Assert, in a form surviving into debug builds, that
the discount is never applied at a depth other than one; that assertion is the
guard against the duplicate-state pruning hazard described above, and it is
cheap.

The statistic needs no change: Milestone 1 counts qualifying SWAPs
unconditionally, so the same instrument measures baseline and improvement.

### Milestone 4 — measure and decide

Run the Milestone 2 loop with the discount disabled and then at several values,
such as one half, one quarter and one seventh, the last being what "this SWAP is
certainly bare" means under the fidelity-weighted cost model. Compare
distributions across at least thirty seeds, never individual runs.

Then decide, and record the decision whichever way it goes. If the qualifying
count rises materially and the total cost does not, keep the option and note
what discount value to recommend. If the count barely moves, say so, recommend
removing the option, and leave this plan in place carrying the finding.

## Concrete Steps

For Gate 0, work from `/Users/yudong/Documents/projects/qec-rydberg-ions` and
run the analysis through `uv run python`. The inputs are
`scripts/test_cases/4.in` and `5.in` and the three checkpoint files named in the
orientation section. Expect the script to print, per routing, the number of
SWAPs, the number that qualify, and how many of those were left bare.

For everything else, work from `/Users/yudong/Documents/projects/mqt.core`.

Establish a baseline before editing:

    cmake --preset release
    cmake --build --preset release --target mqt-core-mlir-unittest-mapping
    ./build/release/mlir/unittests/Dialect/QCO/Transforms/Mapping/mqt-core-mlir-unittest-mapping

Expect a transcript ending in a line reporting all tests passed; record the
count. After Milestone 1 and again after Milestone 3, rebuild and rerun; the
count must be the baseline plus whatever tests this plan adds, because Milestone
1 changes no behaviour and Milestone 3 leaves its option at one in every
pre-existing test.

Build and run the Rydberg-ion binary, which is where the seed loop lives:

    cmake --build --preset release --target mqt-core-mlir-unittest-rydberg-ions
    ./build/release/mlir/unittests/Dialect/QCO/Transforms/RydbergIons/mqt-core-mlir-unittest-rydberg-ions

Expect its existing tests to pass unchanged and the new loop to print its
summary.

If `.agent/plans/rydberg-ion-routing-tool.md` has been completed by the time
Gate 4 is reached, prefer that tool for the sweep, since it varies the discount
without a rebuild. It is not a prerequisite.

## Validation and Acceptance

Gate 0 has no automated test; its acceptance is the recorded finding, and its
failure mode is a premise that does not hold, which stops the plan.

For Milestone 1, add two GoogleTests to
`mlir/unittests/Dialect/QCO/Transforms/Mapping/test_mapping.cpp`.

The first proves the counter is right on a case whose answer is known by hand.
Construct a target and a circuit small enough to trace manually, containing a
two-qubit gate on a pair of qubits followed by a situation forcing the router to
swap that same pair, and assert the statistic reports one. Then insert a
single-qubit gate on one of the two qubits between the gate and the swap and
assert the statistic reports zero. That second case is what proves the any-arity
condition is enforced rather than accidentally satisfied.

**Superseded, 2026-09-26, at the user's direction:** this could not be built
through the full pass; see `Surprises & Discoveries` for why (every minimal
circuit's shared qubit gets measured before the reused SWAP, itself correctly
disqualifying it) and `Decision Log` for the replacement.
`IsGateAdjacentSwapPredicate` unit-tests `isGateAdjacentSwap` directly instead,
covering the matching-pair, mismatched-pair, both-null, and
one-site-touched-since (standing in for the intervening single-qubit
gate/reset/measurement) cases.

The second proves Milestone 1 changed nothing observable: route the same circuit
twice within one process with identical options and assert the SWAP count is
identical and the statistic is no greater than it. The stronger guarantee, that
output matches the previous commit byte for byte, cannot be asserted in a test
because output varies between process launches; check it by hand by running the
Rydberg-ion binary several times before and after and confirming the SWAP counts
fall in the same range.

For Milestone 3, add two more tests.

The first asserts inertness at the default: route a small circuit twice with
identical options except that the second sets `gateAdjacentSwapDiscount` to one
explicitly, and assert the routed modules are structurally identical.

The second asserts the discount changes a decision. Construct a target and
circuit in which two candidate first SWAPs are equally good under the existing
cost model and exactly one of them acts on the site pair of the gate that has
just executed. With the discount at one the choice is arbitrary; with the
discount well below one the qualifying SWAP must win. Assert on the operands of
the first emitted SWAP, following the assertion style the existing
`qubitTypeLabels` test uses. Enable one of the existing cost heuristics in this
test, or rely on the new activation flag being correctly wired — and if the test
fails, check that first, because this is exactly the failure the third entry in
`Surprises & Discoveries` predicts. Construct the circuit so the two candidates
are genuinely tied under the old model, otherwise the test proves nothing about
the new term.

The behavioural acceptance for the plan as a whole is the Gate 4 measurement:
over at least thirty seeds, enabling the discount must increase the mean
qualifying count and must not increase the mean total SWAP cost. If the count
barely moves, that is a legitimate outcome, must be recorded, and must carry a
recommendation about whether to keep the option. Do not tune the test circuit
until the measurement looks good; the measurement is the experiment, not the
acceptance gate.

## Idempotence and Recovery

All build and run steps are repeatable and nothing outside the build directory
is modified. The Gate 0 script only reads.

The risk concentrated in Milestone 3 is the interaction with the search's
duplicate-state pruning. If the discount is ever applied at a depth other than
one, two nodes with the same layout can carry different costs and the pruning
will discard the cheaper one, producing worse routings in a way that is hard to
notice because the output is still correct. The debug assertion required above
is the guard; keep it.

The likeliest correctness bug in Milestone 1 is forgetting to reset the tracking
vectors between routing passes, because `generateLayout` runs several refinement
passes over the same program before the real one. The symptom is a count that is
too high and that changes when `niterations` or `ntrials` changes even though
the final routing did not. Reset them where the routing state is otherwise
initialised, and include a test that runs with `ntrials` greater than one.

If the mapping tests regress after Milestone 1, look first at whether updating
the tracking vectors changed control flow in `advance()`; keep those updates to
pure assignment with no early returns and no conditions beyond the arity check.
If they regress after Milestone 3, confirm the option is at its default in those
tests and that the guarded arithmetic reduces exactly to the original
expression.

## Artifacts and Notes

The reasoning behind the rule, recorded so a later reader need not reconstruct
it. A SWAP is three CZ gates on this hardware, and a protected SWAP is nine,
arranged as three bare SWAPs through an ancilla, the third of which crosses a
diagonal --- see the orientation section, because that leg is what makes a
protected SWAP cost 7.56 rather than 3.00. Protection is needed when a single
fault on one of the two qubits could spread onto the other and produce a
correlated error of weight two that the code cannot correct. If a two-qubit gate
of the original circuit has just coupled exactly those two qubits, that path
already existed and the original circuit was designed to tolerate it, so the
SWAP adds no new uncorrectable path.

A related circuit rewrite, which is not what this plan does but which the
`Decision Log` records as a follow-up sharing this plan's predicate, is
`CircuitOptimizer::cancelCNOTs` in `src/circuit_optimizer/CircuitOptimizer.cpp`
at approximately lines 1182 to 1202, which rewrites a controlled-NOT followed by
a SWAP on the same pair into two controlled-NOTs. It belongs to the repository's
legacy non-MLIR code path and is not wired into this pipeline. The hand-compiled
circuits are direct evidence that this rewrite is worth having: their own source
comment, under the heading `CNOT-SWAP CANCELLATION`, records that the reference
they were transcribed from writes each read-and-step-past move already in the
fused two-controlled-NOT form, and that the file untangles it back into a gate
plus a plain SWAP only so that the gate list is readable. The reference
compilation, in other words, takes this saving as a matter of course while the
MLIR pipeline cannot take it at all.

A caveat that applies to Gate 0 and to every number derived from the simulator:
the verdicts come from a greedy, order-dependent, single-pass search, so they
are an upper bound on how many SWAPs genuinely require protection rather than an
exact count. The two differently-ordered checkpoints for `4.in` are the means of
estimating how large that effect is.

## Interfaces and Dependencies

No new libraries. Milestone 1 additionally touched `MLIRQCOUtils`
(`mlir/include/mlir/Dialect/QCO/Utils/Drivers.h`), beyond the
`MLIRQCOTransforms` and tablegen scope originally stated here:
`walkProgramGraph` gained an optional `skipped` callback, defaulting to nothing,
to observe the single-qubit/reset/measure operations it silently traverses past,
which `advance()` needed for the arity-one tracking recorded in
`Surprises & Discoveries`. No other existing caller of `walkProgramGraph` (e.g.
`getWindow`) passes this callback, so their behavior is unchanged. Gate 0 uses
only the sibling repository's existing `src/mapper_output.py` and checked-in
data.

This plan has no prerequisite ExecPlans. It interacts with two:
`.agent/plans/cost-weighted-trial-selection.md`, which is why measurement keeps
`ntrials` at one until that has landed; and
`.agent/plans/rydberg-ion-routing-tool.md`, which would be a better instrument
for Gate 4 but is not required.

At the end of this milestone sequence the following must exist.
`struct Statistics` in `Mapping.cpp` carries a qualifying-SWAP counter beside
`nswaps`. `def MappingPass` in `Passes.td` declares a statistic
`numGateAdjacentSwaps` with command-line name `num-gate-adjacent-swaps` and an
option `gateAdjacentSwapDiscount` with command-line name
`gate-adjacent-swap-discount` defaulting to one. The generated
`MappingPassOptions` struct gains one correspondingly named field, and the
existing caller in `mlir/lib/Compiler/TargetCompilation.cpp`, which constructs a
default `MappingPassOptions{}`, must continue to compile unchanged. A seed-loop
test exists in the Rydberg-ion test file. `Outcomes & Retrospective` records the
Gate 0 finding, the Gate 2 baseline and the Gate 4 result.
