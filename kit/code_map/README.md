# Function atlas

The [regex implementation audit](REGEX_PROOF.md) records the counted-graph fold,
its complete source accounting, regression evidence and runtime tradeoffs.
The [native shared-floor profile](FREQUENCY_FLOOR.md) ranks remaining performance
work by measured CPU share and distinguishes it from static source reach.
The [diagnostic fold](ERROR_FOLD.md) records the shared reporting entry, source
and binary reductions, and preserved error-policy boundaries.

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

The source digest is
`d1d85a493f8394abf7d80b5516495e6579613ef42e6598c7dbb774d4c12e80f2`.
The generated artifact records its checkout commit and each source file's
SHA-256; during integration the digest identifies the working source even when
the commit still names its baseline. The following measurements are pinned to
that source snapshot. Classification completeness is not correctness evidence.

| Inventory | Count |
| --- | ---: |
| Production files | 81 |
| Production physical lines | 191,006 |
| Ordinary C bodies | 3,647 |
| Generated C entries | 148 |
| C aliases | 100 |
| Assembly symbols, including aliases; architecture variants grouped | 350 |
| Total production symbols | **4,245** |
| Semantic production families | 403 |
| Individually reviewed bodies | 252 |
| Classifications derived from family context | 3,654 |
| Classifications derived from aliases or generators | 339 |
| Additional support entries | 1,575 |

The map contains 146 source files including the consolidated support layer. Production
C coverage equals the sealed 3,895-entry inventory exactly. The 350 assembly
symbols comprise 336 in the library inclusion graph, eight additional platform
signal/setjmp symbols, and six Canvas/kernel symbols. Architecture variants are
shown inside each symbol. The additional platform symbols were outside the
earlier library seal; they are now visible and explicitly classified.

Every production entry has a family, role, responsibility, confidence and review
basis. Family constraints are shared research requirements; individual contract
notes are recorded where evidence supports them. The 252 reviewed bodies cover
12,751 attributed lines, or 9.0% of all function-attributed source. Classification
coverage is complete against this inventory; individual body review is partial.
Only the added reporting entry and its shared ABI boundary received a fresh
three-architecture assembly review in this refresh.

### Where the source lives

These areas are disjoint; AWK is included in utilities.

| Area | Physical LOC | Share |
| --- | ---: | ---: |
| Utilities | 79,332 | 41.5% |
| Shell language and runtime | 36,998 | 19.4% |
| C standard library | 22,219 | 11.6% |
| Assembly library | 18,870 | 9.9% |
| Terminal, editor and system tools | 9,573 | 5.0% |
| Canvas | 7,480 | 3.9% |
| Platform and ABI | 7,257 | 3.8% |
| Common C helpers | 3,812 | 2.0% |
| Network protocols | 2,291 | 1.2% |
| Kernel and Spark | 1,858 | 1.0% |
| Program entries | 716 | 0.4% |
| Bowl runtime | 600 | 0.3% |

The largest files are `src/sh/file.c` (19,682 lines), `src/library.c` (18,870),
`src/sh/text.c` (18,018), `src/sh/util_linux.c` (13,525),
`src/sh/builtin.c` (13,346) and `src/sh/tools.c` (12,197).

### Largest C families

This table uses function-attributed physical lines, including comments and
blanks inside spans. Declarations and other residual source are separate.

| Family | Entries | Attributed LOC |
| --- | ---: | ---: |
| Parameter lookup and transformations | 41 | 2,202 |
| Jobs, waits and terminal process groups | 62 | 1,792 |
| Difference engine | 41 | 1,675 |
| Declarations and variable listings | 38 | 1,600 |
| History storage, expansion and fc | 35 | 1,594 |
| Stream editor | 31 | 1,510 |
| Pattern search command | 30 | 1,391 |
| Directory listings | 30 | 1,222 |
| Shell command grammar | 38 | 1,207 |
| Editor editing operations | 35 | 1,191 |

These ten families total 15,384 lines: **12.2% of the 126,152 lines attributed
to C entries**. The largest 25 account for 24.1%. Size is distributed across
many features; a large reduction will probably require a design shared across
families or several complete subsystem replacements. That is an inference from
the distribution, not a measured deletion estimate. Assembly-family rankings
are available separately; numeric rendering, for example, includes multiple
architecture implementations and distinct conversion contracts.

Source accounting conserves the whole tree:

```text
191,006 production physical lines
  = 141,297 attributed to function/symbol spans
  +  17,707 other code-bearing lines
  +  22,831 other comment lines
  +   9,171 other blank lines
```

The 49,709-line residual includes tables, declarations, macro templates and
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
python3 test/code_map.py -v
python3 kit/function_audit.py --check
node test/code_map_browser.mjs artifacts/function-map-2026-09-08
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
