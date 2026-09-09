# Shared hardware-floor priorities

Native audit of production at `e8e9792`, 2026-09-08. Source digest:
`ef3419884580a4403419fa5f1b270faad1e9ccee0f79d1ea91b233ff337d55a6`.
This establishes an optimization queue, not a speedup or an optimality claim.

## Measured priority

The strongest broad target is **small copying and byte searching**, ahead of
adding new character-class or hash assembly. These operations already have
shared assembly implementations; improving their calls, data flow or existing
bodies can reach many consumers without introducing another primitive family.

| Priority | Existing operation | Sampled share of subject user cycles |
| --- | --- | --- |
| 1 | `memory_copy_apart` | 47.6% sed literal, 37.6% sed captures, 40.5% rev, 39.1% od/hexdump, 25.1% sort, 12.4% AWK fields, 6.4–8.6% shell call/loop/parameter cases |
| 2 | `memory_first_of` | 43.6% cut fields, 19.4% AWK fields, 15.9% uniq, 14.2% sed literal |
| 2 | `string_first_of` | 16.6% shell parameters, 12.3% shell loop, 8.7% shell function calls |
| 3 | `string_span` | 4.6–8.5% across the five shell cases |

These percentages have different per-workload denominators. They cannot be
added into a stack-wide saving. Source reach is a separate measure:
`memory_copy_apart` has 145 lexical C calling functions in 33 files;
`memory_first_of` has 56 in 21; the bounded and terminated class spans together
have 72 in 19. Constant-size specializations, aliases, compiler-emitted calls,
callbacks and conditionally compiled paths make lexical counts incomplete.

The copy samples are concentrated in short paths: 91% of sed literal's copy
samples and 95.6% of rev's land at the second load in the 8–15-byte path.
Ordinary cycle sampling can skid; this does not prove that instruction itself
causes the stall. The first experiment must separate settled input from input
just produced by byte/word stores, and measure actual copy sizes and callers.
Fusing production into the final output buffer may remove more work than
changing a two-load copy sequence. Widening bulk SIMD alone would miss this
observed workload.

An isolated instrumented build confirms the small-call distribution on the
same inputs. All 20 instrumented runs matched the ordinary binary's output,
status and stderr. Counts include transfers into the assembly entry, including
tail calls, and exclude compiler-inlined operations. They are not timings.

| Workload | Copy entry executions | Requested sizes |
| --- | ---: | --- |
| sed literal | 4,333,556 | 81.9% are 8–15 bytes; 666,666 are zero bytes |
| sed captures | 7,000,220 | 41.2% are 8–15; 1,333,332 are 48 bytes |
| sort | 3,002,446 | 96.0% are 8–15 bytes |
| rev | 2,000,223 | 96.1% are 8–15 bytes |
| AWK fields | 3,000,319 | 64.1% are 8–15 bytes |
| shell loop | 4,190,048 | Essentially all below 16 bytes |
| od hex | 1,965,688 | 982,843 each of 16 and 49 bytes |
| hexdump | 2,948,534 | 982,843 each of 16 and 79 bytes, plus short offset formatting |

Caller counts expose data-flow candidates as well as assembly candidates:
sed literal copies each record through `text_reader_spill`, into its pattern
storage, and through `buffered_write_core`. Rev copies through the reader and
writer. AWK copies overwhelmingly through `awk_text_new`. Dump workloads copy
input rows and completed formatted rows. Some copies preserve mutable storage
or refill lifetimes, so removal requires proving those ownership contracts.
Use these distinct distributions in the copy benchmark; the 8–15-byte result
does not describe od/hexdump.

A controlled native 14-byte experiment keeps the producer and copy instructions
the same while directing the preceding stores either to an independent buffer
or to the copied source. Median full-iteration cost rises from 11.2 to 17.6
TSC ticks for byte stores, and 9.5 to 16.4 for word stores plus a byte tail.
This supports testing producer/copy fusion before changing the small-copy
instruction sequence. These are diagnostic counter ticks, not PMU core cycles,
an isolated instruction latency or a production speedup. The
[copy probe](../../artifacts/frequency-floor-2026-09-08/copy-probe/report.md)
retains the controls, layouts, raw repetitions and limitations.

The terminated class span also matters more than source counts suggest:
the shell loop and function workloads each enter `string_span` about 9.9
million times, but enter `string_span_max` only 11 and 14 times. Cut and AWK
each enter `memory_first_of` about four million times, with 75% of requested
bounds at most 32 bytes. Do not optimize only long bounded class scans.

For search, separate short misses, early hits and long scans before changing
dispatch. `exec_keep_value` also searches an entire assignment for `[` before
rejecting a match past the name span; a bounded name search deserves its own
experiment. For class spans, record set identity and returned run lengths
before paying for vector preparation on every short name or blank run.

## Large local sinks

- Fixed-group grep spends 58.9% in `rx_run` and 9.2% in `rx_single`. A separate
  exact event probe for `(ab){32}` records 130 VM iterations and 32 continuation
  writes/reads to match 64 bytes. Faster shared byte scans do not remove that
  repeated interpreter work.
- Function redefinition spends 31.1% in `memory_span_byte`. The allocator scans
  occupancy gaps to find the highest fit; reverse selection may avoid scanning
  most of the map. This targets definition churn, not ordinary function calls.
- Literal grep count, wc words, tr case and base64 respectively spend 96.6%,
  99.8%, 94.9% and 97.3% in their existing fused count/translate/codec assembly.
  These are strong specialized throughput targets, but not four independent
  broad-stack primitives to add. CRC is similarly local: 99.2% in its PCLMUL
  core on this host. Hash and generic formatting did not reach 1% individually
  in these cases; source popularity alone would have ranked them too highly.

## Scope and reproducibility

Twenty deterministic workloads ran natively on a Ryzen 9 9950X, Linux 7.1.8,
GCC 16.2.1. `perf record -e cycles:u -c 500003` sampled the normal optimized,
LTO-linked multicall program. Only symbol stripping was removed. Independently
building the ordinary release binary produced identical `.text`, `.rodata`
and `.data` hashes. Python launcher samples are excluded by the subject DSO;
the reported values are self-cycle shares, without call-graph attribution.

Every workload first matched host-tool output, status and stderr. The regular
file used for timed output was also hashed against validated output. An initial
`/dev/null` trial was rejected: grep detects discarded output and short-circuits.
`sh test/run bench tools` now uses a regular file for both validation and
timing too.

The cases are resident synthetic workloads, not a measured distribution of
user activity. CPU affinity does not isolate the SMT sibling. One machine and
one sampling pass establish leads; paired repeated end-to-end timings are
required to accept any optimization. User-only cycles omit kernel and I/O wait.
No native ARM64/RV64 timing or physical Canvas/kernel profile was collected.
The current host lacks `/dev/spark`; existing Canvas phase counters cannot be
combined into one additive terminal-rendering profile.

Raw samples, source reach, scripts, output hashes, build equivalence and the
independent audits are in
[`artifacts/frequency-floor-2026-09-08/`](../../artifacts/frequency-floor-2026-09-08/).
`profile.py`, `summarize.py`, `profiles.json` and `environment.json` reproduce
and explain the native CPU table. Separate instrumented event counts are
diagnostics, never production timings.

## Acceptance rule for the next floor

First eliminate unnecessary scans/copies at demonstrated hot callers. Then
prototype the remaining shared kernel against its measured argument mix.
Require a paired native end-to-end improvement in multiple consumers before
claiming broad impact. Preserve constant-size compiler specialization, exact
bounds and overlap contracts, kernel register-state restrictions, and all
three baseline architecture bodies. Test forced fallback paths and guard pages;
QEMU supplies correctness evidence, not native speed. Count all new assembly,
adapters and deleted caller code before claiming a LOC reduction.

In particular, `memory_copy` tail-jumps into `memory_copy_apart` for forward-safe
overlap. A replacement cannot infer disjointness from the latter's public name
and interleave head stores with tail loads that still need the original bytes.
