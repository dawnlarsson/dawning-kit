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
| One delimiter, bounded | record, field, sort-key, grep, and path scans | Already folded through `memory_first_of`, `memory_last_of`, `memory_count`, or `string_span_max`. |
| Folded compare/search | `sort -f`, exact `diff -i`, grep literal `-i` | Already folded through `memory_compare_ascii_case` and `memory_search_ascii_case`; scalar converter calls lose badly. |
| Whitespace / word classification | AWK numeric parsing, regex, sort `-d/-i`, option parsers | Caller predicates differ (blank vs space vs printable vs word). Inlined range tests stay cheaper than an out-of-line scalar class call in byte-at-a-time state machines. |
| Escaping and quoting | `ls` hostile names, sed scripts, AWK strings, xargs input | Caller-specific state machines: escape alphabets, delimiters, octal rules, and output ownership differ. Chunk scans already use bounded search where semantics permit it. |
| Two-stop delimiter scan | AWK paragraph split | Only one material caller remains; adding a public routine would inflate the ABI without deduplicating a second loop. |
| Case-converting copy | AWK `tolower` / `toupper`, find `-iname` scratch paths | Kept fused. The in-place floor would require copy plus conversion, adding a full memory pass; a distinct copy-transform ABI has not cleared a caller-shaped benchmark. |
