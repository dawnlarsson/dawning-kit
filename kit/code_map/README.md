# Function atlas

The [regex implementation audit](REGEX_PROOF.md) records the latest architectural
fold, its complete source accounting, regression evidence and runtime tradeoffs.

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
classifications to complete body review.

The source digest is
`6d44543536e94d9038353a7d793e132917e21689283d96247878f34f01244d96`.
The generated artifact records its checkout commit and each source file's
SHA-256; during integration the digest identifies the working source even when
the commit still names its baseline. The following measurements are pinned to
that source snapshot. Classification completeness is not correctness evidence.

| Inventory | Count |
| --- | ---: |
| Production files | 81 |
| Production physical lines | 192,522 |
| Ordinary C bodies | 3,648 |
| Generated C entries | 148 |
| C aliases | 100 |
| Assembly symbols, including aliases; architecture variants grouped | 349 |
| Total production symbols | **4,245** |
| Semantic production families | 403 |
| Individually reviewed bodies | 270 |
| Classifications derived from family context | 3,636 |
| Classifications derived from aliases or generators | 339 |
| Additional support entries | 1,570 |

The map contains 232 source files including the tracked support layer. Production
C coverage equals the sealed 3,896-entry inventory exactly. The 349 assembly
symbols comprise 335 in the library inclusion graph, eight additional platform
signal/setjmp symbols, and six Canvas/kernel symbols. Architecture variants are
shown inside each symbol. The additional platform symbols were outside the
earlier library seal; they are now visible and explicitly classified.

Every production entry has a family, role, responsibility, confidence and review
basis. Family constraints are shared research requirements; individual contract
notes are recorded where evidence supports them. The 270 reviewed bodies cover
16,185 attributed lines, or 11.3% of all function-attributed source. Classification
coverage is complete against this inventory; individual body review is partial.
Assembly instruction bodies were not freshly re-audited for this map.

### Where the source lives

These areas are disjoint; AWK is included in utilities.

| Area | Physical LOC | Share |
| --- | ---: | ---: |
| Utilities | 80,422 | 41.8% |
| Shell language and runtime | 37,427 | 19.4% |
| C standard library | 22,219 | 11.5% |
| Assembly library | 18,843 | 9.8% |
| Terminal, editor and system tools | 9,573 | 5.0% |
| Canvas | 7,474 | 3.9% |
| Platform and ABI | 7,257 | 3.8% |
| Common C helpers | 3,812 | 2.0% |
| Network protocols | 2,291 | 1.2% |
| Kernel and Spark | 1,858 | 1.0% |
| Program entries | 746 | 0.4% |
| Bowl runtime | 600 | 0.3% |

The largest files are `src/sh/file.c` (20,333 lines), `src/library.c` (18,843),
`src/sh/text.c` (18,072), `src/sh/util_linux.c` (13,660),
`src/sh/builtin.c` (13,571) and `src/sh/tools.c` (12,299).

### Largest C families

This table uses function-attributed physical lines, including comments and
blanks inside spans. Declarations and other residual source are separate.

| Family | Entries | Attributed LOC |
| --- | ---: | ---: |
| Parameter lookup and transformations | 41 | 2,202 |
| Jobs, waits and terminal process groups | 63 | 1,850 |
| Difference engine | 41 | 1,689 |
| History storage, expansion and fc | 35 | 1,634 |
| Declarations and variable listings | 38 | 1,616 |
| Stream editor | 31 | 1,510 |
| Pattern search command | 30 | 1,403 |
| Directory listings | 30 | 1,225 |
| Shell command grammar | 38 | 1,207 |
| Editor editing operations | 35 | 1,191 |

These ten families total 15,527 lines: **12.2% of the 127,666 lines attributed
to C entries**. The largest 25 account for 24.0%. Size is distributed across
many features; a large reduction will probably require a design shared across
families or several complete subsystem replacements. That is an inference from
the distribution, not a measured deletion estimate. Assembly-family rankings
are available separately; numeric rendering, for example, includes multiple
architecture implementations and distinct conversion contracts.

Source accounting conserves the whole tree:

```text
192,522 production physical lines
  = 142,791 attributed to function/symbol spans
  +  17,706 other code-bearing lines
  +  22,846 other comment lines
  +   9,179 other blank lines
```

The 49,731-line residual includes tables, declarations, macro templates and
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
