# Establishing worthwhile architectural reductions

Research date: 2026-09-08. Source baseline and measured sizes are in the
[atlas README](README.md). Comparators below are primary sources. Proposed
applications to this repository are inferences, not upstream claims or verified
savings. No external implementation has been transplanted or benchmarked here.

We can establish where source costs concentrate and whether a concrete
replacement improves them under a fixed contract. A source map cannot establish
that all implementations are optimal. The useful unit of investigation is a
complete representation and its producers, consumers and ownership rules.
Identical loops are only one kind of duplication.

## 1. Command metadata, configuration and initialization

This is the strongest next experiment for a reduction spanning many commands.
It is broader than the option-effects loop already costed in the preceding
audit. That narrow replacement was larger than the sampled original; changing
its label to “data-driven” would not change that result.

Toybox's command declaration, generated command inventory, per-command
`GLOBALS`, option flags and initialization cooperate as one design. Its option
parser captures values into command state. The author's architecture walkthrough
explains these boundaries and the generated files.
[Toybox code walkthrough](https://www.landley.net/toybox/code.html).

The experiment here should jointly replace option descriptors, captured-value
storage, configuration declarations, defaults, conversions and initialization
for representative commands. `utility.options` is only 193 attributed lines;
most caller-specific work sits in command entrypoints. The 26,082 lines assigned
the entrypoint role include algorithms and output policy, so they are not an
option-parsing savings estimate.

Select commands covering ordinary final-value options, an ordered `.seen`
action, dynamic/owned values and DD's immediate per-occurrence validation. Count
new descriptors and generated source policy explicitly. Preserve help-before-
validation, diagnostic ordering, repeated options, final stored values and
state reset when commands are invoked repeatedly in the same process. Reject a
design that retains both `file_taking` and an equally large second configuration
representation. A complete pilot should demonstrate net savings before broad
migration.

BusyBox documents shared allocation, IO and option helpers in `libbb`.
That supports examining caller policy alongside mechanism. It does not justify
replacing recoverable errors in an in-process shell with fatal wrappers.
[BusyBox FAQ](https://busybox.net/FAQ.html).

## 2. Parsed-program ownership and expansion results

The largest C family is parameter lookup/transformation: 2,202 attributed lines.
The related declaration, function-lifetime and grammar families expose several
representations and ownership transitions. They overlap conceptually with other
research questions and must not be summed into a deletion promise.

QuickJS directly emits stack bytecode without first constructing a parse tree;
its documented implementation also computes per-function stack limits. Its C
interface makes value ownership explicit. These are concrete design choices to
compare with our retained AST and value lifetimes, not evidence that shell
semantics fit the same representation.
[QuickJS implementation documentation](https://bellard.org/quickjs/quickjs.html).

Two related experiments have different purposes:

- **Persistent parsed programs:** compare reclaimable owned blocks or a linear
  owned program against the current retained arenas and copy/release walkers.
  The preceding audit reproduced an actual defect: alternating definitions of
  two functions exhaust retained storage at iteration 186, while repeating one
  definition 240 times succeeds. This finding is preserved in
  `artifacts/audit-2026-09-08/largest-wins/shell/counterexamples.json`. It is a
  correctness reason to investigate ownership, not a verified large LOC win.
  Keep active self-redefinition, self-unset, nested surviving functions,
  heredoc ownership and canonical function serialization working.
- **Prepared expansion targets and explicit field results:** compare one target
  resolution and result representation against parallel lookup/modifier/emission
  paths. Preserve side-effecting subscript evaluation exactly once, unset versus
  empty, quoted empty fields, array/positional boundaries and quoting provenance.
  An owning word IR must pay for source retention and all consumers; replacing
  byte markers with another object alone does not establish a reduction.

A new VM may add more code than it removes. First cost the owner representation
and its callers separately from instruction dispatch. Measure allocations and
hot execution paths as well as source size.

## 3. Regex and sed representation boundaries

The regex family has 1,353 attributed lines; grep and sed contain substantial
non-regex policy. These are already consumers of one shared regex engine, so
their complete sizes cannot be added as duplicate implementations.

Russ Cox's discussion of production RE2 explicitly questions the recursive
regexp-plus-walker representation and describes postfix form with computed
maximum depth as an alternative. His VM article also explains the state required
for matching and captures. These are useful representation comparisons.
[RE2 implementation discussion](https://swtch.com/~rsc/regexp/regexp3.html),
[regexp virtual machines](https://swtch.com/~rsc/regexp/regexp2.html).

The research boundary is compile representation, traversal state and consumer
interfaces. Preserve longest-match policy, winning captures, backreferences,
nullable loops, resource exhaustion and ordered substitution effects. Sed
already compiles commands and resolves labels; its address ranges, pattern/hold
spaces and output side effects are separate state. A compact tutorial engine
with a smaller grammar is not a feature-equivalent LOC comparison.

## 4. Typed formatting plans across all adapters

musl's `vfprintf.c` uses a state table to turn length modifiers and conversion
characters into argument types, with separate argument fetching and rendering.
That is a concrete prepared-conversion boundary.
[musl vfprintf source](https://git.musl-libc.org/cgit/musl/plain/src/stdio/vfprintf.c).

The useful comparison here includes standard C, shell and AWK argument adapters,
conversion planning and field emission. Preserve shell `%b`, `%q` and `%T`,
format reuse, AWK coercion and standard varargs/error contracts. Earlier core-
only estimates were 107–242 net lines; this research has not turned them into a
large fold. Revisit only if the complete adapter boundary eliminates additional
representations without merely adding a second dispatcher.

## 5. Diff intermediates and Linux inventory models

Diff is a 1,689-line family. Git's xdiff source separates matching preparation,
forward/backward search, changed-line state, edit-script construction and
boundary adjustment. These stages provide a useful comparison of live state
and output policy.
[Git xdiff implementation](https://github.com/git/git/blob/master/xdiff/xdiffi.c).

Our diff already has bidirectional frontier search (`diff_meet`) and discard
heuristics. “Switch to Myers” is not a new proposal. Investigate which of line
indexes, equivalence classes, kept/real mappings, changed flags and edit records
must coexist. Cost a replacement preserving matching tie breaks, whitespace
normalization, binary/equal-input shortcuts, incomplete final lines, ignored
changes, recursive ordering and hunk grouping. Include memory profiles and
adversarial input pairs.

Separately, CPU/block-device reporting and namespace tools suggest two local
models: immutable observation/relationship snapshots for discovery, and ordered
actions with cleanup for namespace changes. Existing `ul_table` and mount-record
ownership are already shared. A new model must remove repeated state while
retaining missing facts, lazy metadata, deterministic parent selection, procfs
races, helper cleanup and authorization ordering. These are repository-derived
research questions, not confirmed large folds or one shared Linux protocol.

## What the clone scan can establish

NiCad's authors support function and block granularity, configurable
normalization, identifier renaming and near-miss comparison. That illustrates
why exact textual matching alone is insufficient.
[NiCad authors' repository](https://github.com/CordyJ/Open-NiCad).

The atlas runs its own whole-body token heuristic, not NiCad. At this snapshot it
finds one consistent-renaming group (`split_same_input`, `csplit_same_input`,
28 combined source lines) and one near pair (`netlink_address_add`,
`netlink_route_add`, 65 combined lines). Those figures are combined source,
not deletion estimates. Sparse output is not evidence that architectural folds
are exhausted: block duplication, macros, ownership protocols and alternative
representations are outside this scan. Running an external clone detector would
first require adapting and validating parsing for this project's C dialect and
generator macros.

## How to accept a candidate

1. Select a complete boundary from the map. Name its old producers, consumers,
   data structures, diagnostics and lifetime rules. Include residual declarations
   and tables in the budget, not only functions.
2. Record the observable contract and source baseline. Reuse existing behavior
   fixtures; add counterexamples for differences the redesign could introduce.
   Separate intended bug fixes from behavior-preserving changes.
3. Implement one complete replacement in isolation. Migrate every counted
   caller and remove the old path. Include new tables, adapters, generated
   artifacts and cleanup in the cost. Avoid counting moved code as deleted.
4. Compare net physical LOC and lexer tokens, plus allocations, runtime and
   binary size where the representation affects them. Report test/tooling LOC
   separately. Preserve supported architectures and the hardware floor's
   performance contracts; their repetition is not automatically duplication.
5. Use differential behavior, failure/lifetime probes and appropriate platform
   checks to validate the replacement. Independently review the complete diff.
   Expand the migration only after both savings and contract preservation hold.

For the user's requested large wins, a net reduction of at least 500 production
lines is a useful experiment target inherited from the preceding audit, not a
claim that any candidate currently achieves it. Prioritize the full command
configuration pilot first for breadth. Treat the retained-program defect as a
separate correctness priority. Keep the expansion and regex representation
experiments next; use the map to bound them before implementing another broad
abstraction.
