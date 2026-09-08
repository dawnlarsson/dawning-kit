# Plan for major architectural folds

**Execution update, 2026-09-08:** the regex experiment qualified and was applied.
It removes 766 production lines including the completion re-audit; source tokens
are essentially unchanged (17 fewer, including directives).
See [implementation, validation and measured tradeoffs](REGEX_PROOF.md).
The remaining proposals are still held. The text below preserves the original
planning estimates and decision gates against their pinned baseline.

This is a planning audit, not a production rewrite. Source is pinned to
`10d043364f0f3b40e34e09848621e1588ee11faa8b2e16209c37e32c2584a029`.
All 80 production files match the function atlas. Test-runner work proceeded
concurrently; it is outside this plan's source changes and savings.

**Recommendation:** do not launch a broad production rewrite on these estimates.
If we pursue one size experiment, use a bounded, isolated regex prototype. It is
the strongest remaining candidate in the boundaries examined, but the corrected
optimistic budget clears 500 lines by only four. The costed seven-command
configuration design adds code after shared support; broader rollout lacks
evidence. Parameter expansion has real correctness problems, with a smaller
estimated size benefit.

| Candidate | Measured old envelope | Unimplemented net reduction | Decision |
| --- | ---: | ---: | --- |
| Complete regex representation | 1,756 LOC | 186–504 LOC | At most a bounded prototype; only four optimistic lines beyond 500 |
| Parameter query/result representation | 1,386 LOC | Central 93; favorable 234; adverse 107 added | Separate correctness project |
| Full configuration for seven commands | 1,405 LOC | 104 added in the costed scenario | Hold broad rollout |

These are competing planning estimates, not measured patch deletions or proof
of global optimality. Three agents challenged complete boundaries; the root
checked accounting, consumer dependencies and other tempting targets. The atlas
covers the whole production inventory, but this pass did not individually reread
every body. The largest ten C families contain only 12.3% of C-attributed lines:
source cost is distributed, so none of these proposals promises a dramatic
reduction across the entire 193,288-line production tree.

## Selection rule

A family-size ranking tells us where to inspect. To select a rewrite, identify
one complete representation that can disappear, including its construction,
mutation, traversal and cleanup. Cost the replacement together with all retained
adapters, declarations and caller migrations. A partial sketch is evidence that
an interface is expressible, not evidence of a working replacement or a measured
source reduction.

Use 500 net production lines as the provisional target for a major size project.
A smaller redesign can still deserve a separate correctness priority. Report
physical lines, nonblank lines and source tokens including preprocessor bodies;
a generator or compressed initializer must not hide its source costs.

Supported language/command behavior, error ordering and ownership are contracts.
Internal instruction layout and exact historical capacity ceilings are design
choices. A bounded representation that handles more input may intentionally
improve those limits. Conversely, do not preserve a demonstrated accidental
second dynamic-variable read simply because the current binary performs it.
Every intended behavior change needs its own reproducer and explicit review.

## Regex: replace the representation, then prove the saving

The current compiler inserts and relocates instructions, copies counted groups,
and reconstructs derived global state when selecting saved programs. The proposed
replacement publishes an immutable expression graph: sequences link nodes,
alternatives point to children, and a counted node stores bounds and one child.
A program owns its prepared search metadata; each match owns captures,
continuations, progress guards and work accounting. Existing library searches,
byte sets and ASCII policies remain shared.

The complete old envelope is 1,756 physical lines, 1,430 nonblank lines and 7,599
source tokens including directives. It includes static declarations, compiler,
matcher, metadata, grep bytecode inspection, conditional save/restore and AWK
checkpoint state, in these inclusive ranges:

| File | Ranges |
| --- | --- |
| `src/sh/text.c` | 754–2311; 2333–2401; 14158–14210; 15246–15253 |
| `src/sh/exec.c` | 8772–8827 |
| `src/sh/awk.c` | 957–959; 980–985; 1000–1002 |

The common `text_literal_find` and grep's output-side prefilter storage remain.
Other consumer edits are charged below even where the current public API may
avoid changing them; they receive no old-line deletion credit. Relocation,
repetition and analysis helpers alone total only 334 lines, so a narrow helper
rewrite cannot reach the target.

| Complete replacement component | Optimistic LOC | Conservative LOC |
| --- | ---: | ---: |
| Types, arenas, compile/match state and ownership API | 175 | 210 |
| Dialect grammar, brackets, classes, escapes and groups | 365 | 425 |
| Graph construction and counted-group lowering | 90 | 120 |
| Matcher, capture undo, progress and choice continuations | 300 | 375 |
| First/last-byte and literal metadata | 80 | 105 |
| Search driver and match-selection policies | 72 | 90 |
| Compile validation and publication | 65 | 80 |
| Grep mandatory-literal and capture-necessity adapters | 45 | 70 |
| All other consumers, checkpoints and reset wiring | 60 | 95 |
| **Total** | **1,252** | **1,570** |

The implied reduction is **186–504 lines**. To remove 500, the entire replacement
must fit in **1,256 lines**, including every adapter. The midpoint fails that
gate. The representation header is only an interface sketch; no executable
replacement, replacement token count or performance improvement is claimed.

The difficult cost is capture-aware continuation/undo plus the complete dialect
parser. A small endpoint-only recursive matcher does not establish this budget.
Equal-end alternatives retain the first depth-first capture assignment; captures
must unwind on failed continuations; nullable mandatory copies must not suppress
the first optional empty copy. WORD matching compares virtual wrapper endpoints.
Metadata must conservatively handle backreferences, nullable branches and anchors.
Grep's mandatory-literal skipping and capture-free fast paths remain performance
obligations. nl/sed retained programs and AWK's kept/dynamic caches require stable
ownership; a compile output must not alias saved mutable metadata.

Independent peer review found no invalid graph invariant, but required two
interface completions: capture demand belongs to each invocation, and a match
can coexist with a pending exhaustion diagnostic. A result enum alone cannot
carry both. The match context must retain those states across the required
search sequence, including first-success exhaustion for exact callers. Compiler
cursor/escape/error state is transient; pool marks govern publication lifetimes.
These obligations are part of the state/matcher/adaptation budgets, not free
implementation details. Back-links must not enter the graph's owning traversal.
The revised state/API sketch is already 155 physical lines before setup bodies;
charging a further 20–55 lines raises that component to 175–210. This correction
reduces the optimistic margin from 59 lines to four. The matcher estimate remains
unproved and must not absorb new code without recounting it.

The initial sketch embedded 768 bytes of metadata in each program. Review changed
the proposal to small descriptors referencing owned pooled metadata, including
checkpoint and failed-compile publication rules. This avoids multiplying inline
tables across saved-program arrays, but still needs BSS/stack measurement.

Intentional capacity changes are permitted with tests and a documented bounded
work policy. Exact old instruction/frame exhaustion thresholds would need an
extra estimated 60–120 lines of compatibility accounting, reducing the planning
range to 66–444. That policy would eliminate the current 500-line scenario.
Distinguish malformed patterns from exhausted matching in either design, and
explicitly decide the exact-match caller's exhaustion observation point.

## Full utility configuration: broad rollout is not justified yet

This audit expanded the previous effects-only proposal to include option
metadata, typed configuration, defaults, capture, validation and initialization.
It counted 151 `file_taking` initializers: 992 lines. The literal metadata contains
378 required-value keys and 64 optional-value keys, with two additional
conditional schemas. These are capture keys, not removable conversion blocks.
There are 22 occurrence hooks and 19 supersession tables.

Seven complete setup envelopes were measured:

| Command | Old setup LOC | Proposed complete local LOC | Local reduction before shared support |
| --- | ---: | ---: | ---: |
| flock | 82 | 74 | 8 |
| pr | 298 | 276 | 22 |
| numfmt | 171 | 121 | 50 |
| env | 236 | 230 | 6 |
| sort | 307 | 287 | 20 |
| setpriv | 145 | 147 | -2 |
| dd | 166 | 134 | 32 |
| Total | **1,405** | **1,269** | **136** |

Old ranges are measured; replacement costs are a complete planning scenario,
not a patch delta. Adding 240 lines of shared schema, conversion, phase,
scanner-adaptation and macro support gives **104 lines added** for this sample.
The 147-line schema sketch is incomplete and is not counted as a complete engine.
Its token count rises from 485 to 843 when its macro body is included.

The architecture can encode these commands, but their different policies still
need code: PR's deferred final values, DD's immediate conversion, ordered
setpriv/env/sort effects, ownership, exact diagnostic subjects, optional values,
help-before-validation and command-specific setup phases. Domain state such as
sort keys and PR geometry remains even after option capture is shared.

**Decision:** do not start a utility-wide migration based on the initializer
count. A smaller numfmt proof can challenge these estimates later. A broad
rollout requires a working complete pilot and separately costed eligible callers
whose combined net savings pay for the shared mechanism. Thirty-eight metadata
sites have at least three literal required-value keys and no `.seen` hook; that
is a triage list, not proof of conversion similarity or 38 profitable migrations.

Independent review found no double charge or concrete large missing deletion.
It did identify an incomplete accounting detail: the shared scanner's +35-line
net adaptation estimate lacks an exact old-global/new-global range ledger.
Separate fixed support from marginal command costs in any proof; the first
command need not repay the entire engine. Even zero shared cost leaves only
136 planned lines removed for these seven paths, holding their local estimates
fixed. That sensitivity check is not a lower bound on another implementation.

The utility registry already uses one 197-row `tools.inc` catalog. Its 235-line
catalog/projection envelope is not a new fold to claim again.

## Shell parameter results: a concrete correctness target

A prepared query and explicit scalar/sequence result would replace repeated
lookup, joining, modifier setup and field routing. The complete selected old
boundary is 1,386 lines in `src/sh/expand.c`, including `expand_value_of`,
`expand_braced`, sequence providers and the affected modifier adapters.
Transformation algorithms, quoting marks, assignment policy and diagnostics
remain necessary and are charged separately from the new dispatcher.

Fresh probes against the attested x64 shell found supported Bash-mode gaps:

```sh
set -- aa bb
printf '<%s>\n' "${@//a/X}"
# Current: one field <XX bb>; Bash: <XX> then <bb>.

RANDOM=7
printf '%s:%s\n' "${RANDOM-default}" "$RANDOM"
# Current: 26956:7409; Bash: 19344:26956.

a=(aa aa); i=0
printf '<%s>\n' "${a[@]//a/$((i+=1))}"
printf 'i:%s\n' "$i"
# Current: <11>, <22>, i:2; Bash: <11>, <11>, i:1.
```

The replacement must express evaluation order, not assume every target is an
identical snapshot. Scalar substring/replacement keeps the scalar value before
operand side effects. Array operations prepare operands before borrowing the
live inventory; new elements created by a modifier operand can participate.
Empty sequences can skip operand evaluation. Quoted positional boundaries must
survive until the existing marked-field emitter consumes them.

Dynamic `RANDOM@Q` remains a policy question: Bash returns the first draw but
also advances its state further than a plain read. It cannot be grouped blindly
with the clean default-operator duplicate-read case. Indirect reassignment,
nameref attributes and readonly diagnostics also need individual policy review.

The complete parameter replacement is estimated at 1,152 / 1,293 / 1,493 lines
in favorable / central / adverse scenarios: **234 removed / 93 removed / 107
added**. The 242-line scheduling model contains 203 production-intended lines
and 39 model-only lines; the latter are excluded from the production budget.
All real providers, kernels and diagnostic/caller adapters are charged. These
are planning estimates, not measured deletions.

**Decision:** pursue this only as an explicitly scoped correctness and design
project; do not sell it as a 500-line size fold. First preserve the
26 exact counterexample/control scripts, then implement direct query/result
providers behind existing expansion entrypoints. Migrate scalar/default paths,
then positional and array modifiers, then remove the old duplicated acquisition
paths. Validate each stage against the supported shell modes and failure/arena
lifetimes. Keep assignment mutation, the field splitter and the hardware floor
as the existing shared implementations.

The retained-AST arena defect remains a separate ownership issue: alternating
function redefinitions strand old blocks. The preceding audit's owning-program
model is not an implemented shell fix. A whole word VM, declaration-engine
replacement or jobs/history merger does not inherit a large savings estimate
from this parameter work.

## Other large-looking targets that did not hold

| Boundary examined | Why this pass does not justify a major rewrite |
| --- | --- |
| Diff, 1,928-line complete envelope | Bidirectional Myers plus frontier storage is already 112 lines. Normalization, discard heuristics, tie shifting and hunk policy are distinct work; replacing them changes output or complexity. |
| Sed, 1,716-line complete envelope | Already one flat command VM with indexed labels and pointer-swapped buffers. A parser table retains address state, input cycles, substitution and transactional output. Sharing regex's execution state would add side-effect rollback. |
| Record reader/cursor, 438 lines | Already shared. comm pairs, join duplicate products, paste stdin aliasing and uniq streaming have different lifetime and error/publication ordering. No 500-line deletion exists in this selected boundary before replacement. |
| Declaration, jobs/waits and history mergers | Declarations already share a mutation engine. Job terminal state and retained child results have different lifetimes. History substitution already shares its normal transform; its pre-parser quoting and persistent state do not become parameter-expansion semantics. No complete large replacement was established. |
| Editor/terminal action sample, 529 lines | A 500-line reduction would leave 29 lines for all effects, state ordering and bindings. Numeric parameter parsing and editor motion tables are already shared. |
| Selected standalone format/scan glue, 263 lines | Too small for this target; standalone formatting is an actively tested configuration. This is a selected glue sample, not a bound on all formatting designs. |

These findings reject the named proposals under this objective; they do not
prove the subsystems minimal. Canvas, kernel and architecture-specific assembly
remain covered by the atlas, but were not freshly instruction-audited here and
receive no new savings claim. Do not count removing supported architectures,
utilities, fast paths or tests as a behavior-preserving fold.

## Execution plan and stop conditions

1. **Freeze the regex contract and oracle.** Preserve the source manifest and
   baseline binary. Record direct match slots, return/error state, compile
   diagnostics and resource policy. Specify intentional capacity changes
   separately from language behavior. Reuse the existing test infrastructure
   and preserve exact counterexamples; do not refresh expected output silently.
2. **Implement the complete candidate in isolation.** Include dialect parsing,
   nodes, matching, metadata, ownership and concrete adapters for every counted
   consumer. Make the capture request and match/error context concrete first.
   Keep the production engine unchanged during this experiment.
   No feature stubs or extrapolation from a partial matcher establish savings.
   Stop the major-size proposal if integrated replacement costs exceed 1,256
   readable physical lines; do not force the target by packing statements.
3. **Compare behavior and resource use.** Exercise nested/retained captures,
   backreferences, malformed and repeated quantifiers, nullable groups,
   equal-end choices, FIRST/LONGEST/EXACT, WORD/LINE, and success followed by
   later exhaustion. Cover grep, nl, sed, AWK, shell `BASH_REMATCH`, expr, tac,
   csplit, ptx and column, including cache rollover and saved-program lifetimes.
   Measure literal searches, anchored concatenations, byte/group repeats and
   adversarial `(a|aa)*` over `a^N + z`; preserve the proven last-byte upper bound
   that prevents the prior longest-search blowup. Record compile/match runtime,
   allocation/maximum work storage, stack, binary text and BSS.
4. **Have an independent reviewer try to reject it.** Require at least 500 net
   production LOC removed, fewer source tokens including macro bodies, complete
   caller migration and no unexplained correctness or performance regression.
   Report test/tooling and generated-source costs separately. Build supported
   architectures and run their available relevant checks; document any runtime
   coverage gap. A smaller successful patch may be reconsidered on its actual
   merits, but does not meet the major-size target by changing accounting.
5. **Only then replace production and re-audit.** Remove the old engine and
   transitional adapters, run complete relevant consumer regressions, verify
   the final diff against the source ledger, and refresh the atlas. If the
   proof fails, record the failed budget/contract and stop this proposal.

## Evidence and verification

The working evidence is under `artifacts/major-fold-plan-2026-09-08/`:

- `source-manifest.json`: SHA-256 and physical size of every production file.
- `engines/assessment.md`, `ledger.json`, `measure.py`, `representation.h`,
  `peer-review.md`: exact competing boundaries, regex costs, contracts, storage
  sketch and independent interface review.
- `options/report.md`, `ledger.json`, `initializer-inventory.json`,
  `effect-sites.json`, `representation-sketch.c`, `peer-review.md`: complete
  setup scenarios, metadata census and independent challenge.
- `shell/report.md`, `ledger.json`, `parameter-plan.c`, `phase-probes.py`,
  `phase-probes.json`: costed parameter model and 26 current/Bash probes.
- `root/review.md`, `boundary-ledger.json`, `accounting.py`, `validation.json`:
  alternative boundaries and independent source/accounting checks.

The inventory seal passed. Old source hashes, disjoint selected ranges and
scenario arithmetic were independently checked; a missed regex adapter closing
brace was corrected before finalizing this plan. Planning sketches were checked
for C11 syntax only. No production rewrite was implemented or benchmarked in
this pass, and no production LOC reduction is claimed.
