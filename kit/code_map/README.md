# Function atlas

The [regex implementation audit](REGEX_PROOF.md) records the counted-graph fold,
its complete source accounting, regression evidence and runtime tradeoffs.
The [native shared-floor profile](FREQUENCY_FLOOR.md) ranks remaining performance
work by measured CPU share and distinguishes it from static source reach.
The [diagnostic fold](ERROR_FOLD.md) records the shared reporting entry, source
and binary reductions, and preserved error-policy boundaries.
The [wrapper-removal pass](WRAPPER_REMOVAL.md) removes the remaining thin
diagnostic helpers and records the resulting source and binary tradeoffs.
The [performance recovery pass](PERFORMANCE_RECOVERY.md) measures the combined
regex, text-reader, shell and shared-search changes against the prior binary
and installed reference tools.
The [shell performance pass](SHELL_HARDWARE.md) removes duplicate token storage,
redundant arithmetic scans, quoted-field preparation and unnecessary process
and pathname lookups, with final measured gains and remaining gaps.

The [shell and utility continuation](SHELL_CORE_PERFORMANCE.md) qualifies retained
alias storage, numeric parsing and shared assembly against a fresh baseline
after the concurrent utility compatibility merge.

This is a source map for finding and costing architectural reductions. It joins
the sealed production inventory to semantic classifications, source extents,
possible call relationships and whole-function similarity leads. It does not
certify correctness or establish that the implementation is minimal.

Build and open the standalone map:

```sh
python3 kit/code_map/build.py
open artifacts/function-map-2026-09-08/index.html
```

Python 3 and Git are sufficient to generate it. No server, package installation
or external web resource is needed to view it. `--out PATH` changes the output
directory. The output includes HTML source snapshots with line anchors,
`functions.csv`, `functions.json` and `summary.json`.

Use **Files** to locate total source costs, **Code sinks** to rank semantic
families, and **Functions** to inspect responsibilities, signatures, review
evidence, contracts, possible callers/callees and shared helpers. Filter by
area, family, role or evidence; switch between attributed physical lines,
code-bearing lines and C lexer tokens. **Similarity leads** links candidate
pairs back to their contracts and source. **Coverage & method** explains scope
and limitations. The Files view keeps whole-file totals when function filters
select containing files.

## Audit trail

Local reports under `artifacts/` retain their original baseline, counts and
verdicts. Use the later implementation record when an earlier review says
"unfixed" or "unapplied"; those historical labels are not the current backlog.

| Effort | Status and follow-up |
| --- | --- |
| [Codebase audit and verification](../../artifacts/audit-2026-09-08/verification-2/verification.md) | C01–C16, cached enable lookup, and the coupled small folds were repaired in `009bce0`; [implementation record](../../artifacts/audit-2026-09-08/implementation-1/implementation.md). |
| [Utility and arithmetic reductions](../../artifacts/audit-2026-09-08/reduction-2/reduction.md) | Applied in `de27dfd`: file loops, literal replacement, typed columns, mount/blkid policy and arithmetic lvalues. |
| [Binding, sort and regex search designs](../../artifacts/audit-2026-09-08/architecture-1/architecture.md) | Applied in `ae8ef2c`; regex storage/execution was subsequently replaced by the counted graph. |
| [Larger fold research](FOLD_PLAN.md) | Planning estimates are preserved. The regex prototype qualified; other proposed large replacements remain unqualified. |
| [Counted regex and completion audit](REGEX_PROOF.md) | Applied in `92359fd` and `c6f5a6a`; 766 production lines removed, with recorded performance and resource tradeoffs. |
| [General tidy](../../artifacts/general-tidy-2026-09-08/review.md) | Removes 46 further production lines and 172 source tokens: unused shell APIs/state and the previously proposed D13 dd operand table. Repairs stale sed state and lsblk include paths in maintained audit fixtures. |
| [Storage and teardown repairs](../../artifacts/resolve-2026-09-08/review.md) | Reuses retired function-body ranges, streams mapfile records, and releases orphaned Canvas cursor buffers. Removes 69 production lines overall; adds 622 tokens for ownership and correctness checks. |
| [Wrapper removal](WRAPPER_REMOVAL.md) | Removes 55 C functions, four logging macros and 449 production lines; centralizes diagnostic transport and prefix/flush selection in `library.c`. Callers gain 5,738 lexer tokens and the normal binary grows 11,248 bytes. |
| [Shell and utility continuation](SHELL_CORE_PERFORMANCE.md) | Fresh baseline, allocation reuse, numeric scan removal and assembly layout qualification; historical rejected experiments remain separate. |
| [Shell performance](SHELL_HARDWARE.md) | Broad shell workloads, retained-token ownership, arithmetic/field work deletion, numeric kill and terminal glob syscall removal. The shared-span assembly experiment is rejected. |
| [Hardware bounds](HARDWARE_FLOOR.md) | Conditional hardware limits, actual core cycles, wider shared permutations/table lookup, and consumer batching with before/after qualification. |
| [Performance recovery](PERFORMANCE_RECOVERY.md) | Fixed regex proofs reuse shared search assembly; reader/output fusion removes copies; reverse arena selection skips lower gaps; short byte searches avoid feature dispatch. Paired native results and resource tradeoffs are recorded separately. |
| [Diagnostic fold](ERROR_FOLD.md) | Removes 1,401 production lines and 830 lexer tokens. Adds one assembly entry sharing the formatter across three architectures; removes diagnostic aliases and repeated report/return blocks. |

The [mapfile follow-up](../../artifacts/resolve-2026-09-08/mapfile/review.md)
distinguishes invalid explicit descriptors from Bash-compatible open-descriptor
read errors and fixes count/origin/consumption behavior. Alternating function
redefinitions now reuse free ranges while active calls retain their entered
bodies. [Canvas ownership](../../artifacts/cleanup-2026-09-08/canvas/review.md)
is repaired and fault-tested against an atomic DRM ownership model. Presentation
folding remains unqualified; no live-display or physical device-failure test was
performed. No historical estimate is counted as a deletion.

## What the current map establishes

Classification began at `ae8ef2c3aa346a64fbd832a9c90608b3296fa069`.
The first refresh mapped the counted-regex replacement and its consumer migration.
It removed 44 obsolete C entries, added 25 individually reviewed graph/compiler
functions, and refreshed 505 location IDs by file/name mapping. Unchanged bodies
keep their earlier classifications. Four changed AWK bodies retain their family
classification but are conservatively excluded from complete body-review totals.
The moved `text_literal_find` retains its earlier annotation. The completion
re-audit removes no further functions and refreshes 130 location IDs. Changed
regex bodies were re-read; targeted caller changes do not promote family
classifications to complete body review. The general tidy pass removes two
unreferenced shell functions and remaps 654 location IDs after deleting dead
shell state and folding dd numeric operands. Four targeted dispatch/signature
notes retain their existing family classifications.
The storage follow-up removes four obsolete parser helpers, adds five reviewed
storage helpers, and remaps 308 location IDs. Existing retained-body functions
were re-read against the replacement. Targeted mapfile, executor and Canvas
notes retain their family classifications; the old top-only storage constraint
has been replaced by the current range and reference-count contracts.
The diagnostic pass remaps 2,090 location IDs and adds the reviewed assembly
entry `string_report`. Twenty changed C bodies are conservatively returned to
family classification: their diagnostic changes were reviewed, while the full
surrounding functions were not re-reviewed in this pass.
The stricter wrapper-removal pass deletes 55 C entries, adds three reviewed
assembly entries and remaps 2,329 location IDs. Twelve changed bodies return to
family classification because only their changed paths were re-reviewed.
The performance passes add one reviewed regex proof constructor, remap 556
location IDs and return 4 changed bodies to family classification. The
changed paths and their bounds/lifetimes received independent review; this does
not promote the complete surrounding bodies.
The shell performance pass removes 3 C helpers, remaps 304
location IDs and returns 5 changed bodies to family classification.
Targeted lifetime, parser, field and syscall contracts are recorded without
promoting the entire shell to complete body review.

The concurrent utility merge and this continuation remove 6 old entries, add 120
family-context classifications and one reviewed private assembly entry and remap 1,502 location IDs.
4 changed bodies lose stale individual-review status. The new utility
entries are classified from their source sections and established families;
this does not claim a complete correctness audit of the merged implementations.

The source digest is
`e76f319cdb726afdb6696281401eac8fc92368d645370d03a36f2e908912a348`.
The generated artifact records its checkout commit and each source file's
SHA-256; during integration the digest identifies the working source even when
the commit still names its baseline. The following measurements are pinned to
that source snapshot. Classification completeness is not correctness evidence.

| Inventory | Count |
| --- | ---: |
| Production files | 81 |
| Production physical lines | 197,492 |
| Ordinary C bodies | 3,706 |
| Generated C entries | 146 |
| C aliases | 100 |
| Assembly symbols, including aliases; architecture variants grouped | 354 |
| Total production symbols | 4,306 |
| Semantic production families | 403 |
| Individually reviewed bodies | 228 |
| Classifications derived from family context | 3,741 |
| Classifications derived from aliases or generators | 337 |
| Additional support entries | 1,497 |

The map contains 140 source files including the consolidated support layer. Production
C coverage equals the sealed 3,952-entry inventory exactly. The 354 assembly
symbols comprise 340 in the library inclusion graph, eight additional platform
signal/setjmp symbols, and six Canvas/kernel symbols. Architecture variants are
shown inside each symbol. The additional platform symbols were outside the
earlier library seal; they are now visible and explicitly classified.

Every production entry has a family, role, responsibility, confidence and review
basis. Family constraints are shared research requirements; individual contract
notes are recorded where evidence supports them. The 228 reviewed bodies cover
9,929 attributed lines, or 6.8% of all function-attributed source. Classification
coverage is complete against this inventory; individual body review is partial.
The performance pass reviewed the x86-64 short-search dispatch and kernel gate;
its unchanged ARM64/RV64 bodies retain their earlier review evidence.

### Where the source lives

These areas are disjoint; AWK is included in utilities.

| Area | Physical LOC | Share |
| --- | ---: | ---: |
| Utility implementations | 85,542 | 43.3% |
| Shell language and runtime | 37,107 | 18.8% |
| C standard library | 22,205 | 11.2% |
| Assembly library | 19,099 | 9.7% |
| Terminal, editor and system tools | 9,565 | 4.8% |
| Canvas | 7,460 | 3.8% |
| Platform and ABI | 7,257 | 3.7% |
| Common C helpers | 3,812 | 1.9% |
| Network protocols | 2,291 | 1.2% |
| Kernel and Spark | 1,838 | 0.9% |
| Program entries | 715 | 0.4% |
| Bowl runtime | 601 | 0.3% |

The largest files are `src/sh/file.c` (23,474 lines),
`src/library.c` (19,099 lines),
`src/sh/text.c` (17,976 lines),
`src/sh/builtin.c` (13,648 lines),
`src/sh/util_linux.c` (13,620 lines),
`src/sh/tools.c` (13,397 lines).

### Largest C families

This table uses function-attributed physical lines, including comments and
blanks inside spans. Declarations and other residual source are separate.

| Family | Entries | Attributed LOC |
| --- | ---: | ---: |
| Directory listings | 71 | 2,434 |
| Parameter lookup and transformations | 40 | 2,191 |
| Difference engine | 44 | 1,783 |
| Jobs, waits and terminal process groups | 59 | 1,775 |
| Find expression and traversal engine | 34 | 1,767 |
| Declarations and variable listings | 35 | 1,633 |
| History storage, expansion and fc | 35 | 1,593 |
| Stream editor | 31 | 1,505 |
| Pattern search command | 30 | 1,388 |
| Shell command grammar | 38 | 1,207 |

These ten families total 17,276 lines: **13.1% of the 131,738 lines attributed
to C entries**. The largest 25 account for 25.0%. Size is distributed across
many features; a large reduction will probably require a design shared across
families or several complete subsystem replacements. That is an inference from
the distribution, not a measured deletion estimate. Assembly-family rankings
are available separately; numeric rendering, for example, includes multiple
architecture implementations and distinct conversion contracts.

Source accounting conserves the whole tree:

```text
197,492 production physical lines
  = 147,060 attributed to function/symbol spans
  +  18,107 other code-bearing lines
  +  23,032 other comment lines
  +   9,293 other blank lines
```

The 50,432-line residual includes tables, declarations, macro templates and
assembly scaffolding. It cannot be counted as removable overhead. A replacement
must include its new descriptors and declarations in its measured cost.

See [the research comparisons](RESEARCH.md) and the subsequent
[audited fold plan](FOLD_PLAN.md) for complete replacement budgets and current
priorities. The regex implementation is now represented as counted-graph compilation,
search metadata, and execution/consumer interfaces. Its measured source reduction
is 766 production lines; behavior, resource limits and performance require their
separate qualification evidence. This atlas does not supply that proof.
The annotations contain a design question and constraints for every family,
rather than a savings estimate derived from its size.

## Evidence and metric definitions

- `body_reviewed`: a current body was read for classification, during this
  task or the immediately preceding audit of the identical source. It does not
  mean the function was proved correct. `family_rule` is interpretation from
  family context. `alias_or_generator` describes a named alias or generated
  entry without claiming independent expanded-body review.
- C spans come from the sealed declarators and balanced source bodies. Raw
  configuration alternatives remain visible. Generated entries share their
  invocation; templates remain in residual file costs. Each physical line and
  lexer token is attributed once, even when entry spans overlap.
- C lexer tokens exclude preprocessor directives. Assembly symbol token counts
  are zero by design, so compare assembly with physical/code lines. This is
  neither an instruction count nor binary size. A line containing any code is
  code-bearing; a residual line with only a comment is counted as a comment.
- Calls are lexical possibilities with same-file preference, not compiler
  resolution. Callbacks, member calls and outside-body references are separate.
  Includes, macros, local pointer shadowing, unevaluated expressions and
  configuration guards limit the graph. Zero callers never proves dead code.
- Shared-helper links cover `library.c`, `library.common.c`,
  `compiler_memory.c` and platform assembly. Reuse of higher utility helpers is
  visible through ordinary callees. A missing library link does not prove a
  caller should use one.
- The support layer includes ordinary C bodies and aliases in tracked source;
  test-generating macro expansions are not enumerated. Support classifications
  are file-derived, not the curated production taxonomy.
- Similarity scans whole ordinary C bodies using consistent-renaming exact
  matches and blind-renaming token shingles. Thresholds and candidate caps are
  recorded in JSON. It misses blocks, small bodies, macro expansions and
  assembly. Renaming callees or types can erase crucial semantic differences.
  This is not NiCad or an equivalence analysis.

## Maintaining and checking the map

`annotations/*.json` is curated data, with one function or family per line for
localized diffs. IDs include source path, name and location (assembly IDs group
architecture spans). All annotation sets are pinned to the production source
digest. A changed digest or missing/stale production ID fails a normal build.
`--allow-incomplete` exposes unmapped entries while preparing a new map; it does
not bypass a supplied stale digest. Refresh the affected annotations against
the changed source before updating pins. Do not preserve a `body_reviewed`
claim for a changed body without checking it again.

```sh
python3 test/harness.py code_map -v
python3 kit/function_audit.py --check
node kit/code_map/browser.mjs artifacts/function-map-2026-09-08
```

The browser check uses Node with built-in `fetch`/`WebSocket` and an isolated
headless Chrome profile. Set `ATLAS_CHROME` to another Chrome executable when
needed. It checks scopes, filters, navigation, exports, source links and layout
at three widths in light/dark themes, and writes screenshots and a report beside
the artifact. Its inventory expectations are pinned to this map; deliberately
update them after validating a new source inventory.

Refreshing this map changes tooling, classifications and metrics. The production
regex replacement is a separate change; its source reduction must be counted
across every changed production file, independently of this map.
