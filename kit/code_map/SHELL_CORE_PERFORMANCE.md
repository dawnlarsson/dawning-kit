# Shell and coreutils performance continuation

This pass removes repeated alias allocations, defers unnecessary IFS preparation,
skips special-value scans on ordinary numeric input, and adds a guarded wide
repeated-byte scan inside the existing library interface. The assembly's initial
consumer regressions were diagnosed and repaired through section placement.

The final comparison is against **0941bb7**, after another concurrent effort
merged utility implementations and replaced several test suites. Earlier isolated
experiments use **16b5ef4** and remain explicitly historical. The old directory
optimization was superseded by that merge's cached-width implementation; its
81% isolated gain is not claimed as this final patch's gain. We retain two small
listing correctness repairs against the current engine.

## Measured final behavior

The main maintained suite covers108 workloads (35 shell and73 utility cases),
with11 rotating rounds after two warmups. Nineteen extra controls cover numeric
inputs, alias creation/redefinition/query, and directory columns, with9 rounds.
Every measured invocation matches reference status, stderr and raw stdout bytes;
no invalid binary or output was waived. Inputs and output hashing are outside
timing, the real multicall argv0 is used, and child waits are blocking.

Linux7.1.8, GCC16.2.1, Ryzen9 9950X, CPU15 affinity. Other host/SMT activity was
not isolated. Our build/timing windows were serialized; unrelated processes were
not controlled. Reported CPU is child user+system time; separate wall samples
and every pair are retained. Ratios of medians and medians of pairs can disagree.

| Case | Before CPU ms | After CPU ms | Reference ms | After/before | Paired ratio | Faster rounds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| shell/alias | 113.765 | 12.044 | 45.770 | 0.1059 | 0.1061 | 11/11 |
| shell/alias-redefine | 90.819 | 7.533 | 25.572 | 0.0829 | 0.0835 | 9/9 |
| shell/alias-redefine-equal | 117.578 | 8.462 | 28.128 | 0.0720 | 0.0720 | 9/9 |
| shell/loop | 73.408 | 64.706 | 392.953 | 0.8815 | 0.8804 | 11/11 |
| shell/function | 94.801 | 88.560 | 666.843 | 0.9342 | 0.9343 | 11/11 |
| shell/scalar-long | 40.796 | 35.808 | 580.624 | 0.8777 | 0.8797 | 11/11 |
| awk-fields | 15.026 | 14.533 | 18.697 | 0.9672 | 0.9681 | 11/11 |
| awk-numeric/digits | 15.560 | 15.111 | 18.520 | 0.9711 | 0.9721 | 9/9 |
| shell/parse | 7.127 | 7.219 | 46.624 | 1.0129 | 1.0128 | 2/11 |
| shell/parameter-mix | 74.139 | 75.648 | 373.486 | 1.0204 | 1.0182 | 0/11 |
| sort | 11.489 | 11.511 | 16.806 | 1.0019 | 1.0023 | 5/11 |
| uniq | 3.825 | 3.835 | 11.851 | 1.0026 | 1.0024 | 4/11 |
| dir/8000 | 3.728 | 3.713 | 3.389 | 0.9960 | 0.9960 | 6/9 |

The final median improves in 75/108 main cases and
14/19 extras. It is faster than the installed
reference in 107/108 main cases and
14/19 extras. These counts describe
this corpus and host, not every input or an always-faster guarantee.

`cat` is effectively tied (0.463 versus0.461ms). Extra controls expose a material
remaining gap:4,096 fresh alias definitions take151.005ms versus4.845ms in Bash;
new names/values still receive separate mappings. Existing-alias reuse removes
that allocation cost, but new-definition allocation needs a separate design
change. GNU also remains faster on the larger directory controls.

Parsing rises1.29% and mixed parameter expansion2.04%; their paired ratios are
1.0128 and1.0182, with2/11 and0/11 faster rounds. Base64 decode without wrapping
has a1.0120 paired ratio (3/11 faster); wrapped base2lsbf encoding1.0112 (3/11).
These adverse controls remain visible. Sort/uniq and the cut controls are within
about0.7% at the final medians, with weak paired direction. All127 rows follow
below; no aggregate score replaces individual results.

## Why the changes help

- Alias expansion retains its temporary token array through the existing
  `shell_room_relax` policy. Nested lexer scans cannot execute another alias
  before publication; retained parser tokens/traces own their spelling. A
  diagnostic100-call fixture changes100 mappings/frees to one mapping/no frees.
- Alias definitions look up the terminated name with `string_table_find` before
  allocation. Existing fitting values reuse storage through overlap-safe
  `memory_copy`. Growth copies before freeing, allocation failure leaves the
  prior alias intact, and giant-to-small transitions release oversized storage.
  The caller always restores the temporarily replaced `=` in its owned argv.
  A100-redefinition probe changes200 mappings/frees to zero.
- Empty/wholly quoted fields return before `expand_ifs_prepare`; those paths
  never read IFS. Actual splitting still prepares the current table, including
  changing IFS and nameref cases.
- Only ASCII I/i/N/n can begin infinity/inf/nan. Ordinary numeric prefixes skip
  the pure bounded-length/comparison work. Rounding, NaN payloads, endpoints
  and errno behavior stay on the existing paths. Isolated user instructions
  decrease1.3–2.9% on ordinary AWK numeric rows; special values remain unchanged.
  The explicit shared whitespace-table proposal was rejected: compiler_memory.c
  already prepares and merges those literals, so it saved no data or runtime work.
- Current directory columns round the maximum candidate count upward as pinned
  GNU9.11 does. Two one-byte names at width4 become `a  b\n`; empty horizontal
  listing emits nothing. Existing candidate minima and strict growth-fit rules
  remain unchanged.72 positive GNU checks now pass, up from57 in that baseline.

The detailed [alias/IFS](../../artifacts/shell-core-perf-2026-09-09/shell/report.md),
[alias ownership](../../artifacts/shell-core-perf-2026-09-09/shell/alias-record/report.md),
[numeric](../../artifacts/shell-core-perf-2026-09-09/numbers/report.md) and
[column](../../artifacts/shell-core-perf-2026-09-09/coreutils/current-columns/report.md)
reports retain independent fixtures, source pins and rejected variants.

## Assembly regression repair and hardware limits

The first wide scan improved bulk input while moving unrelated shared routines.
Sort and uniq still produced exact output with the span entry replaced by INT3:
they never execute it in these cases. The old candidate's fresh-composition
user-cycle regressions were2.52% and4.21%, with unchanged retired work.

The corrected implementation uses a shorter SSE2 low-byte broadcast, fits its
large-count gate/trampoline inside existing alignment space, and puts the wide
body in a private executable ELF section. In the isolated comparison, all2,382
other existing text-symbol addresses and sizes remain fixed. The original
short-loop bytes/addresses are preserved. Relocated data references account for
every other changed instruction byte; there are no unexplained interior changes.
Kernel and non-ELF bodies remain identical, and ARM64/RV64 bodies are unchanged.

Grouped actual user-cycle ratios after that repair are loop1.00045,
function0.99690, sort0.99642 and uniq0.99508. Long quoted-field cycles fall7.58%
and instructions11.07% in that isolated comparison. The independent8KiB span
falls1068.738→176.777 actual cycles, about6.05× faster. Short and forced-feature
controls are not uniformly faster: the complete report retains their adverse
rows and context sensitivity, including a forced-SSE31-byte control18→25cycles.

176.777cycles remains2.74× an optimistic64.5-cycle partial bound: under suitable
alignment, L1 residency and available resources, this candidate performs129
vector loads at a documented capacity of two/cycle. Compare throughput, mask
resource sharing, control dependencies and dispatch add constraints; this is
not a complete latency model or proof of attainable/global optimality. Primary
AMD sources, every PMU row and model assumptions are in the
[assembly report](../../artifacts/shell-core-perf-2026-09-09/shared/span-byte-section/report.md).

The new section would survive ordinary ELF linking but disappear from Spark's
flat text extraction. The one-line linker inclusion repairs this packaging path.
The actual packager's output text bytes are verified against its retained ELF,
and both ELF callers execute exact equal/mismatch checks. This does not claim
execution through the Spark kernel loader. The
[packaging proof](../../artifacts/shell-core-perf-2026-09-09/coreutils/spark-span/report.md)
retains both the failing old extraction and the fixed one.

## Correctness and source identity

The isolated ASM qualification passes678,681 exact bound/guard/byte-value/dirty-
upper-argument/forced-tier/kernel checks and21,690 grouped primitive PMU rows.
The current listing corpus includes72 raw GNU cases,12 explicitly labeled
existing-policy baseline controls, and39 larger/error controls; these overlap
and are not added as unique coverage. Alias allocation/overlap/failure probes
pass220 assertions, with32 exact baseline cases and28 applicable Bash cases.
The four documented existing differences are retained.

The concurrently merged `lane_shell` called a helper with a missing argument
and then invoked an empty domain. It now executes the existing shell suites,
lexer-span checks and generated corpora, including four new alias/IFS cases.
The permanent span test adds127/128/129-byte dispatch boundaries. The final
adapter uses current maintained test definitions, independently pinned binaries,
source/test inventories, positive tallies and baseline-on-failure reproduction;
deleted suites are not recreated. A stale test-file transport pin was caught
before execution, corrected by checksum synchronization and reverified.

Final native validation is running; this draft is not a completion claim.

The production inventory seal,26 atlas unit checks and80 browser checks pass.
Classification covers the current inventory; individual body review remains partial.

The patch against0941bb7 changes production source by **+101 / −30, net+71 physical lines**,
with no new C function and one private assembly symbol. This is a performance
addition, not a LOC-removal claim. Existing `.text` grows256bytes; the private
executable section adds323bytes. RO/data/BSS sizes stay fixed. The stripped ELF
grows728bytes (1,514,832→1,515,560). Normal and symbol-preserving builds have
identical bytes in both executable sections and the measured RO/data sections.

| Final identity | SHA256 |
| --- | --- |
| Baseline0941 stock | `caa9d962e79b73d0c69280e7f3a10079c7c8bc277ed08fad15ee336536f36073` |
| Final stock | `fba5928078d82799afd3a3b060a402dcd16ef55ae438036304e4629a6e7e0903` |
| Final symbols | `2ccf82eeed9c2111c6cbccdeb57424c434596ac0db657e840b654ed0c1a616a7` |
| Production atlas digest | `e76f319cdb726afdb6696281401eac8fc92368d645370d03a36f2e908912a348` |
| source inventory file | `1845dc8a32a3b35734c9f24949df3413f9af00168036d029f7c6cdcd8db7551d` |
| test inventory file | `1d5e31fc787aa7bb40bd6cd31acacea13d6f644a86e278f3c5f92056107daf21` |
| baseline-source inventory file | `c14ccb5ffbfb30ce1bba0a64de604e2a541b6bffda17b8448eefe2281e2aea2e` |

The production atlas now includes the concurrent merge's120 family-classified
new C entries and removes six stale entries; those are not this patch's source
additions. Four changed bodies lose stale individual-review status. The atlas
tracks3,952 C entries and354 assembly entries across81 production files; only
228 bodies have individual-review evidence. Its source digest and the build's
compact-JSON inventory digest use different documented definitions.

## Complete final controls

| Case | Before CPU ms | After CPU ms | Reference ms | After/before | Paired ratio | Faster rounds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| base64/encode/w0 | 0.720 | 0.707 | 1.791 | 0.9819 | 0.9874 | 9/11 |
| base64/decode/w0 | 0.581 | 0.592 | 3.190 | 1.0189 | 1.0120 | 3/11 |
| base64/encode/w76 | 0.851 | 0.848 | 2.330 | 0.9965 | 1.0023 | 5/11 |
| base64/decode/w76 | 1.151 | 1.153 | 6.121 | 1.0017 | 0.9931 | 6/11 |
| base64url/encode/w0 | 0.707 | 0.705 | 4.889 | 0.9972 | 0.9929 | 7/11 |
| base64url/decode/w0 | 0.572 | 0.576 | 5.921 | 1.0070 | 1.0197 | 4/11 |
| base64url/encode/w76 | 0.863 | 0.866 | 5.447 | 1.0035 | 1.0035 | 5/11 |
| base64url/decode/w76 | 1.155 | 1.147 | 9.175 | 0.9931 | 0.9974 | 7/11 |
| base32/encode/w0 | 0.863 | 0.858 | 2.444 | 0.9942 | 0.9988 | 6/11 |
| base32/decode/w0 | 0.627 | 0.628 | 3.272 | 1.0016 | 0.9984 | 6/11 |
| base32/encode/w76 | 1.033 | 1.021 | 3.104 | 0.9884 | 0.9892 | 6/11 |
| base32/decode/w76 | 1.619 | 1.627 | 5.549 | 1.0049 | 1.0037 | 5/11 |
| base32hex/encode/w0 | 0.859 | 0.857 | 5.082 | 0.9977 | 0.9930 | 8/11 |
| base32hex/decode/w0 | 0.622 | 0.623 | 6.137 | 1.0016 | 1.0016 | 5/11 |
| base32hex/encode/w76 | 1.017 | 1.015 | 5.748 | 0.9980 | 0.9913 | 6/11 |
| base32hex/decode/w76 | 1.623 | 1.609 | 8.708 | 0.9914 | 0.9901 | 8/11 |
| base16/encode/w0 | 0.900 | 0.904 | 3.371 | 1.0044 | 1.0022 | 5/11 |
| base16/decode/w0 | 0.638 | 0.641 | 3.676 | 1.0047 | 0.9969 | 6/11 |
| base16/encode/w76 | 1.149 | 1.145 | 4.150 | 0.9965 | 0.9913 | 6/11 |
| base16/decode/w76 | 1.444 | 1.466 | 4.393 | 1.0152 | 1.0126 | 4/11 |
| base2msbf/encode/w0 | 3.527 | 3.501 | 13.255 | 0.9926 | 1.0037 | 5/11 |
| base2msbf/decode/w0 | 1.876 | 1.879 | 25.187 | 1.0016 | 1.0217 | 4/11 |
| base2msbf/encode/w76 | 4.521 | 4.430 | 16.692 | 0.9799 | 0.9913 | 8/11 |
| base2msbf/decode/w76 | 7.490 | 7.528 | 25.257 | 1.0051 | 1.0041 | 4/11 |
| base2lsbf/encode/w0 | 3.519 | 3.539 | 13.348 | 1.0057 | 0.9983 | 6/11 |
| base2lsbf/decode/w0 | 1.844 | 1.833 | 22.641 | 0.9940 | 0.9950 | 6/11 |
| base2lsbf/encode/w76 | 4.458 | 4.494 | 16.760 | 1.0081 | 1.0112 | 3/11 |
| base2lsbf/decode/w76 | 7.450 | 7.530 | 25.575 | 1.0107 | 1.0129 | 4/11 |
| seq/integer | 0.956 | 0.944 | 3.845 | 0.9874 | 0.9865 | 8/11 |
| seq/stride | 0.757 | 0.756 | 15.889 | 0.9987 | 1.0000 | 6/11 |
| seq/padded | 0.252 | 0.259 | 23.942 | 1.0278 | 0.9919 | 6/11 |
| seq/decimal | 1.202 | 1.205 | 25.333 | 1.0025 | 1.0000 | 5/11 |
| seq/formatted | 1.235 | 1.234 | 25.670 | 0.9992 | 0.9968 | 7/11 |
| seq/descending | 4.954 | 4.947 | 120.501 | 0.9986 | 1.0032 | 4/11 |
| seq/negative | 5.006 | 5.013 | 122.005 | 1.0014 | 1.0028 | 4/11 |
| seq/literal | 0.217 | 0.210 | 2.342 | 0.9677 | 0.9951 | 7/11 |
| dmesg/timestamp | 1.598 | 1.602 | 9.800 | 1.0025 | 1.0038 | 3/11 |
| dmesg/delta | 2.363 | 2.356 | 11.998 | 0.9970 | 0.9996 | 6/11 |
| dmesg/json | 2.268 | 2.267 | 10.185 | 0.9996 | 0.9974 | 7/11 |
| uuidparse/raw | 0.339 | 0.331 | 1.521 | 0.9764 | 0.9878 | 7/11 |
| uuidparse/json | 0.412 | 0.396 | 1.967 | 0.9612 | 0.9726 | 9/11 |
| shell/parse | 7.127 | 7.219 | 46.624 | 1.0129 | 1.0128 | 2/11 |
| shell/loop | 73.408 | 64.706 | 392.953 | 0.8815 | 0.8804 | 11/11 |
| shell/parameter | 57.066 | 56.199 | 282.801 | 0.9848 | 0.9876 | 10/11 |
| shell/function | 94.801 | 88.560 | 666.843 | 0.9342 | 0.9343 | 11/11 |
| shell/redefine | 39.107 | 31.517 | 142.251 | 0.8059 | 0.8057 | 11/11 |
| cat | 0.467 | 0.463 | 0.461 | 0.9914 | 1.0022 | 5/11 |
| cat-number | 3.190 | 3.158 | 3.496 | 0.9900 | 0.9881 | 7/11 |
| cat-visible | 0.957 | 0.955 | 1.816 | 0.9979 | 1.0053 | 5/11 |
| wc-lines | 0.199 | 0.203 | 0.320 | 1.0201 | 1.0201 | 5/11 |
| wc-words | 0.264 | 0.268 | 2.429 | 1.0152 | 0.9961 | 6/11 |
| grep-literal | 0.328 | 0.327 | 1.762 | 0.9970 | 1.0030 | 5/11 |
| grep-group8 | 0.737 | 0.735 | 0.919 | 0.9973 | 1.0042 | 5/11 |
| grep-group32 | 0.762 | 0.764 | 1.160 | 1.0026 | 0.9987 | 6/11 |
| cut-field | 3.675 | 3.683 | 3.863 | 1.0022 | 1.0016 | 5/11 |
| cut-trim-first | 2.108 | 2.101 | 2.935 | 0.9967 | 0.9971 | 6/11 |
| cut-prefix | 1.945 | 1.940 | 2.702 | 0.9974 | 1.0031 | 5/11 |
| cut-suffix | 2.000 | 2.014 | 2.807 | 1.0070 | 1.0020 | 5/11 |
| tr-case | 0.490 | 0.489 | 1.444 | 0.9980 | 0.9918 | 7/11 |
| tr-delete | 1.380 | 1.379 | 1.554 | 0.9993 | 1.0022 | 4/11 |
| tr-squeeze | 2.748 | 2.752 | 11.582 | 1.0015 | 0.9956 | 7/11 |
| sed-literal | 7.823 | 7.807 | 24.139 | 0.9980 | 0.9948 | 8/11 |
| sed-capture | 14.399 | 14.394 | 79.585 | 0.9997 | 1.0003 | 5/11 |
| awk-fields | 15.026 | 14.533 | 18.697 | 0.9672 | 0.9681 | 11/11 |
| sort | 11.489 | 11.511 | 16.806 | 1.0019 | 1.0023 | 5/11 |
| uniq | 3.825 | 3.835 | 11.851 | 1.0026 | 1.0024 | 4/11 |
| head | 0.120 | 0.117 | 0.177 | 0.9750 | 0.9919 | 6/11 |
| tail | 0.121 | 0.117 | 0.184 | 0.9669 | 0.9669 | 7/11 |
| rev | 3.023 | 2.989 | 29.901 | 0.9888 | 0.9937 | 8/11 |
| base64 | 0.696 | 0.701 | 1.712 | 1.0072 | 0.9986 | 6/11 |
| base32 | 0.847 | 0.836 | 2.341 | 0.9870 | 0.9918 | 8/11 |
| od-hex | 10.773 | 10.769 | 219.105 | 0.9996 | 1.0082 | 4/11 |
| hexdump | 8.285 | 8.258 | 275.857 | 0.9967 | 1.0005 | 5/11 |
| cksum | 0.331 | 0.329 | 0.603 | 0.9940 | 1.0060 | 4/11 |
| sha256sum | 1.808 | 1.808 | 2.162 | 1.0000 | 1.0000 | 6/11 |
| comm | 3.749 | 3.723 | 4.209 | 0.9931 | 0.9933 | 6/11 |
| paste | 2.032 | 2.041 | 2.099 | 1.0044 | 1.0054 | 3/11 |
| join | 7.596 | 7.426 | 14.379 | 0.9776 | 0.9789 | 10/11 |
| shell/startup | 0.095 | 0.096 | 0.383 | 1.0105 | 0.9886 | 6/11 |
| shell/arithmetic | 31.781 | 29.489 | 120.266 | 0.9279 | 0.9244 | 11/11 |
| shell/scalar-short | 30.268 | 28.383 | 144.291 | 0.9377 | 0.9373 | 11/11 |
| shell/scalar-long | 40.796 | 35.808 | 580.624 | 0.8777 | 0.8797 | 11/11 |
| shell/parameter-mix | 74.139 | 75.648 | 373.486 | 1.0204 | 1.0182 | 0/11 |
| shell/fields | 38.530 | 37.018 | 176.974 | 0.9608 | 0.9596 | 11/11 |
| shell/ifs-fields | 38.303 | 37.646 | 178.218 | 0.9828 | 0.9805 | 11/11 |
| shell/positional | 93.968 | 93.122 | 431.727 | 0.9910 | 0.9905 | 10/11 |
| shell/local-nameref | 77.484 | 76.044 | 333.907 | 0.9814 | 0.9786 | 11/11 |
| shell/array-indexed | 37.338 | 34.613 | 144.955 | 0.9270 | 0.9272 | 11/11 |
| shell/array-associative | 50.958 | 50.803 | 197.695 | 0.9970 | 0.9981 | 9/11 |
| shell/builtin-format | 42.160 | 34.784 | 199.192 | 0.8250 | 0.8243 | 11/11 |
| shell/case-pattern | 30.985 | 28.098 | 132.002 | 0.9068 | 0.9054 | 11/11 |
| shell/regex-condition | 77.297 | 75.247 | 454.254 | 0.9735 | 0.9714 | 11/11 |
| shell/glob | 34.457 | 34.422 | 62.549 | 0.9990 | 0.9957 | 8/11 |
| shell/brace | 13.699 | 13.454 | 40.274 | 0.9821 | 0.9807 | 11/11 |
| shell/read-lines | 130.024 | 129.223 | 140.330 | 0.9938 | 0.9938 | 11/11 |
| shell/mapfile | 4.300 | 4.278 | 4.724 | 0.9949 | 0.9958 | 7/11 |
| shell/command-substitution | 11.648 | 11.581 | 23.134 | 0.9942 | 1.0011 | 4/11 |
| shell/pipeline | 20.920 | 20.594 | 42.545 | 0.9844 | 0.9857 | 9/11 |
| shell/subshell | 8.112 | 8.156 | 18.809 | 1.0054 | 0.9999 | 6/11 |
| shell/background-wait | 9.705 | 9.620 | 29.386 | 0.9912 | 0.9887 | 10/11 |
| shell/redirect | 3.788 | 3.721 | 8.430 | 0.9823 | 0.9808 | 9/11 |
| shell/source | 9.190 | 9.180 | 9.943 | 0.9989 | 0.9961 | 7/11 |
| shell/trap | 3.104 | 3.062 | 9.046 | 0.9865 | 0.9865 | 10/11 |
| shell/parse-arguments | 13.936 | 13.234 | 75.747 | 0.9496 | 0.9547 | 11/11 |
| shell/parse-quotes | 38.535 | 37.799 | 133.820 | 0.9809 | 0.9788 | 11/11 |
| shell/parse-grammar | 31.575 | 30.460 | 93.958 | 0.9647 | 0.9606 | 11/11 |
| shell/parse-heredoc | 1.643 | 1.522 | 3.119 | 0.9264 | 0.9249 | 11/11 |
| shell/alias | 113.765 | 12.044 | 45.770 | 0.1059 | 0.1061 | 11/11 |

## Additional controls

| Case | Before CPU ms | After CPU ms | Reference ms | After/before | Paired ratio | Faster rounds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| awk-numeric/digits | 15.560 | 15.111 | 18.520 | 0.9711 | 0.9721 | 9/9 |
| awk-numeric/decimal | 15.984 | 15.648 | 22.012 | 0.9790 | 0.9765 | 9/9 |
| awk-numeric/wide | 18.077 | 17.523 | 24.278 | 0.9694 | 0.9676 | 9/9 |
| awk-numeric/space | 16.087 | 15.167 | 17.790 | 0.9428 | 0.9438 | 9/9 |
| awk-numeric/mixed | 14.950 | 14.757 | 17.826 | 0.9871 | 0.9914 | 8/9 |
| awk-numeric/special | 20.242 | 20.269 | 21.282 | 1.0013 | 1.0022 | 4/9 |
| shell/alias-redefine | 90.819 | 7.533 | 25.572 | 0.0829 | 0.0835 | 9/9 |
| shell/alias-define-new | 150.314 | 151.005 | 4.845 | 1.0046 | 1.0046 | 2/9 |
| shell/alias-query | 11.580 | 11.486 | 28.490 | 0.9919 | 0.9930 | 9/9 |
| shell/alias-redefine-equal | 117.578 | 8.462 | 28.128 | 0.0720 | 0.0720 | 9/9 |
| shell/alias-redefine-grow | 117.200 | 9.622 | 30.782 | 0.0821 | 0.0820 | 9/9 |
| shell/alias-value-large | 3.508 | 0.964 | 34.811 | 0.2748 | 0.2765 | 9/9 |
| shell/alias-value-giant-shrink | 3.700 | 3.585 | 43.715 | 0.9689 | 0.9614 | 8/9 |
| dir/1000 | 0.535 | 0.540 | 0.589 | 1.0093 | 1.0054 | 3/9 |
| dir/1000/reverse | 0.529 | 0.530 | 0.584 | 1.0019 | 0.9962 | 5/9 |
| dir/5000 | 2.347 | 2.359 | 2.156 | 1.0051 | 1.0043 | 2/9 |
| dir/5000/reverse | 2.379 | 2.350 | 2.158 | 0.9878 | 0.9870 | 7/9 |
| dir/8000 | 3.728 | 3.713 | 3.389 | 0.9960 | 0.9960 | 6/9 |
| dir/8000/reverse | 3.739 | 3.735 | 3.379 | 0.9989 | 1.0005 | 4/9 |

Raw [main measurements](../../artifacts/shell-core-perf-2026-09-09/integration/results/all.json),
[extra measurements](../../artifacts/shell-core-perf-2026-09-09/integration/results/extra.json),
[build manifest](../../artifacts/shell-core-perf-2026-09-09/integration/results/build-manifest.json)
and [validation](../../artifacts/shell-core-perf-2026-09-09/integration/results/validation-final/results.json)
retain their actual inputs, versions, source/binary identities and outcomes.
The cut batching candidates remain rejected because prefix selection regressed;
the historical directory-cache candidate is superseded, and no such patch was
copied over the concurrent implementation.
