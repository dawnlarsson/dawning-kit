# Hot byte-loop census

This is the read-first ledger for `file.c`, `text.c`, `tools.c`, `expand.c`,
and `awk.c`. A loop is moved into the assembly library only when a direct
caller-shaped benchmark beats current compiler output on x86-64 and Apple
ARM64 without moving another architecture backwards.

| Shape | Main callers | Decision and evidence |
| --- | --- | --- |
| ASCII upper/lower, in place | `dd` case conversions; unpatterned `${x^^}` / `${x,,}` | Folded through `memory_to_lower_ascii` / `memory_to_upper_ascii` at 32 bytes. Per-byte calls were 13–16 times slower. Copy-and-convert AWK values remain fused because a second memory pass loses the gain. |
| 5381 × 33 byte hash | AWK array keys; exact `diff` line classes | Folded through `memory_hash_33` at 24 bytes. Four-byte polynomial reduction is about 1.7× on x86-64 and 1.1–1.3× on M2 at caller spans; short keys remain inline. |
| Equal-byte prefix | AWK decimal digit streams; sort numeric leading-zero runs | `memory_span_byte` clears the 64-byte caller gate: equal/late spans are about 7×/6× faster on x86-64, 2×/1.4× on ARM64, and 1.7× on RV64 under the floor runner. Short runs remain inline. |
| One delimiter, bounded | record, field, sort-key, grep, path, and Canvas line-layout scans | Already folded through `memory_first_of`, `memory_last_of`, `memory_count`, or `string_span_max`. Canvas preserves the extra boundary byte so a newline exactly at the wrap edge retains its old precedence over a word break. |
| Folded compare/search | `sort -f`, exact `diff -i`, grep literal `-i`, HTTP header names | Already folded through `memory_compare_ascii_case` and `memory_search_ascii_case`; scalar converter calls lose badly. HTTP compares a bounded field name, so it shares the bulk comparison directly instead of lowering either input into scratch space. |
| Literal-size folded compare | inlined HTTP header-name tokens and other fixed protocol fields | `memory_compare_ascii_case` expands through 12 bytes: folded/routine is 65–92% on x86-64, 65–75% on ARM64 and 59–83% under the RV64 floor runner. Sixteen is rejected because ARM64 rises to 160%; 24–32 are rejected on both vector floors. |
| Literal bounded strings | HTTP 7/8-byte tokens, resolver 11-byte tokens, 15-byte interface names | Existing bounded specializers were remeasured in those exact shapes. X86-64 is 55–93% and ARM64 51–81%. RV64 deliberately expands only bounds up to 4 for scans and 8 for copy-end; its non-expanded rows stay level with the routine. |
| Whitespace / word classification | AWK numeric parsing, regex, sort `-d/-i`, option parsers | Caller predicates differ (blank vs space vs printable vs word). Inlined range tests stay cheaper than an out-of-line scalar class call in byte-at-a-time state machines. |
| Escaping and quoting | `ls` hostile names, sed scripts, AWK strings, xargs input | Caller-specific state machines: escape alphabets, delimiters, octal rules, and output ownership differ. Chunk scans already use bounded search where semantics permit it. |
| Two-stop delimiter scan | AWK paragraph split | Only one material caller remains; adding a public routine would inflate the ABI without deduplicating a second loop. |
| Case-converting copy | AWK `tolower` / `toupper`, find `-iname` scratch paths | Kept fused. The in-place floor would require copy plus conversion, adding a full memory pass; a distinct copy-transform ABI has not cleared a caller-shaped benchmark. |
| Star beside one byte, trimmed | `${x##*/}`, `${x%/*}`, `${x%%.*}`, `${x#*.}` in expand.c | Folded onto `memory_last_of` / `memory_first_of`: the longest prefix ending in a byte runs to its last occurrence, the shortest to its first, and the suffixes mirror. Trying every cut through the glob matcher was a pass per byte of the value; one hunt answers it. |
| Literal replacement | `${x//lit/rep}` and the anchored forms in expand.c | Folded onto `memory_search_prepare` once and `memory_search_prepared` per occurrence; the anchored forms are one `memory_compare` at the end. The general search asked the matcher about every cut of every position, which a literal never needs. |
| Common suffix | diff's tail scan in tools.c | Not added. It would mirror `memory_common_prefix` byte for byte, but has one caller; cmp already uses the prefix form. Reconsider when a second backwards scan appears. |
| Table-driven byte filter | tr -d / -ds / -s in text.c | Not added. One caller, and a lookup per byte is what the compiler already emits; there is no baseline-floor SIMD compaction to beat it with (x86 without VBMI2, armv8.0 without SVE). |
| Two-stop delimiter, again | awk single-byte FS in paragraph mode | Still one material caller; the here-document body scan that looked like a second folded onto `string_span_max` with a stop set instead. |

