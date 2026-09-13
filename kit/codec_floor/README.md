# Codec performance record — 2026-09-13

This revision moves the measured gzip and xz paths substantially closer to the reference tools and improves zstd compression. **The goal of winning every case and proving a hardware floor remains unmet.** Gzip wins all eight standalone throughput medians in this run, although the source wins are narrow. Xz wins every standalone encode row, but source and binary decode remain slower. Zstd wins source decode and several repeat/random rows; source, binary and random encode still trail.

The measurements are from a Ryzen 9 9950X, Linux 7.1.8-arch1-3, pinned to CPU 6, one compression thread, five measured process invocations after a warm-up. Throughput is uncompressed MiB/s and includes the standalone CLI, I/O and framing. Input is cached; output goes to `/dev/null`. Each encoder's actual output and each decoder's output are byte-checked. **All decoders receive the same reference-compressed input**, including when the table lists a different size for that implementation's encoder.

Before is commit `56de0b11ce16c8f59a087247cbeec438ea6272ab`. Reference versions: GNU gzip 1.14-modified, XZ Utils 5.8.3, Zstandard 1.5.7, GNU tar 1.35. Native compiler: GCC 16.2.1 (20260810); cross compilers: AArch64 GCC 16.1.0 and RISC-V GCC 15.1.0. Boost remained enabled; the measurements are medians from this host, not universal speed guarantees. The JSON records every sample, corpus digest and binary digest.

Standalone encode levels are gzip -6, xz -1 and zstd -3. Tar uses its ordinary codec defaults; tar -J is therefore a different compression-level comparison from standalone xz -1. `XZ_OPT=-T1` and `ZSTD_NBTHREADS=1` also constrain reference tar's child compressors.

## Encode throughput

| Codec / corpus | Before MiB/s | Now MiB/s | Reference MiB/s |
|---|---:|---:|---:|
| gzip / src | 41.3 | 68.4 | 67.2 |
| gzip / elf | 30.4 | 49.6 | 40.0 |
| gzip / rep | 498.4 | 5221.8 | 1102.4 |
| gzip / rnd | 70.3 | 93.2 | 70.4 |
| xz / src | 83.7 | 118.5 | 64.4 |
| xz / elf | 57.7 | 48.9 | 29.7 |
| xz / rep | 661.0 | 3002.4 | 447.0 |
| xz / rnd | 34.9 | 121.1 | 10.3 |
| zstd / src | 305.8 | 373.2 | 526.4 |
| zstd / elf | 166.8 | 197.1 | 362.9 |
| zstd / rep | 1815.2 | 8272.7 | 2722.3 |
| zstd / rnd | 1142.1 | 1043.7 | 2364.3 |
| tar / tar-src | 5620.2 | 6199.7 | 4639.3 |
| tgz / tar-src | 41.3 | 67.8 | 66.1 |
| txz / tar-src | 72.8 | 71.8 | 8.4 |
| tzst / tar-src | 295.9 | 366.8 | 436.5 |

## Decode throughput

| Codec / corpus | Before MiB/s | Now MiB/s | Reference MiB/s |
|---|---:|---:|---:|
| gzip / src | 546.6 | 669.3 | 662.9 |
| gzip / elf | 318.5 | 466.5 | 393.4 |
| gzip / rep | 1999.0 | 4639.2 | 726.9 |
| gzip / rnd | 2302.2 | 7735.7 | 1131.8 |
| xz / src | 141.4 | 261.6 | 391.5 |
| xz / elf | 60.5 | 110.8 | 154.5 |
| xz / rep | 2113.4 | 4549.4 | 707.9 |
| xz / rnd | 2272.5 | 8896.1 | 6382.3 |
| zstd / src | 2074.3 | 2266.3 | 2222.0 |
| zstd / elf | 1157.3 | 1165.1 | 1434.3 |
| zstd / rep | 13401.9 | 13433.7 | 13145.9 |
| zstd / rnd | 8205.3 | 7627.1 | 4490.9 |
| tar / tar-src | 5395.9 | 5895.7 | 4458.4 |
| tgz / tar-src | 505.7 | 613.9 | 559.9 |
| txz / tar-src | 170.2 | 313.1 | 403.5 |
| tzst / tar-src | 1595.2 | 1714.8 | 1341.3 |

## Encoded sizes

The source and binary corpora are 16 MiB each, repeat is 32 MiB, and random is 8 MiB. Lower is better. The tar corpus is generated from the source corpus by the harness.

| Codec / corpus | Before bytes | Now bytes | Reference bytes |
|---|---:|---:|---:|
| gzip / src | 3,428,713 | 3,407,837 | 3,410,190 |
| gzip / elf | 8,816,669 | 6,034,888 | 6,054,807 |
| gzip / rep | 61,194 | 32,661 | 32,594 |
| gzip / rnd | 8,391,279 | 8,391,176 | 8,389,914 |
| xz / src | 3,697,820 | 2,490,916 | 2,185,064 |
| xz / elf | 5,664,124 | 5,564,668 | 5,180,716 |
| xz / rep | 10,896 | 10,360 | 5,016 |
| xz / rnd | 8,389,052 | 8,389,052 | 8,389,088 |
| zstd / src | 3,916,773 | 2,478,024 | 2,487,989 |
| zstd / elf | 6,999,783 | 6,056,010 | 6,028,951 |
| zstd / rep | 2,830 | 1,037 | 1,068 |
| zstd / rnd | 8,388,813 | 8,388,813 | 8,388,814 |
| tar / tar-src | 16,811,520 | 16,811,520 | 16,814,080 |
| tgz / tar-src | 3,467,334 | 3,434,114 | 3,437,974 |
| txz / tar-src | 3,584,676 | 2,296,332 | 1,745,268 |
| tzst / tar-src | 4,001,481 | 2,495,382 | 2,497,553 |

## Implementation and assembly evidence

- Shared CRC: x86 PCLMUL folds four independent 128-bit accumulators for reflected IEEE CRC32 and reflected ECMA CRC64, with a 1024-byte threshold and a CPUID feature gate. Polynomial reduction and bounded tails preserve the existing seed/continuation contract. The x86 CRC32C instruction is not used. Kernel builds and the other architectures retain their scalar table paths.
- Gzip: complete length-limited Huffman codebooks, exact block-cost accounting, pre-reversed codes, a word bit writer, larger token blocks, a 64K hash, cached lazy lookahead and a two-byte match-end filter. The tri-architecture inflate span uses 11/8-bit primary tables and emits two literals between refills. The bound checks still stop at exact input and output limits.
- Xz: a new 144-byte `lzma_decode_span` ABI keeps the LZMA machine, range coder, probabilities, repeat distances and dictionary writes together in assembly on all three architectures. Its admission margin covers a complete packet; malformed distance and chunk overrun checks precede match writes. Framing retains the scalar refill/wrap tail. CRC is calculated once per output slab. Encoding preserves a 1 MiB hash-chain history across 64 KiB LZMA2 chunks and abandons demonstrably expanding chunks early.
- Zstd: a 2 MiB match history, explicit repeat-offset probes, adaptive sequence FSE tables, a four-lane literal histogram, real RLE blocks and reduced redundant short-period hash insertion. Sequence bit extraction is inline and branch-free on all three architectures. Pull readers retain a consumed cursor; raw/RLE block boundaries and missing checksums are handled explicitly. Every sequence's logical output remains bounded.

`reference/` contains the selected C starting points used to generate and inspect the LZMA and inflate assembly. These files are development references and are not linked into the runtime. The production implementations are in `src/library.c`. Compile references with `-O2 -ffreestanding -fno-tree-vectorize -fno-stack-protector -S`; use the repository's target baseline and, on AArch64, `-ffixed-x18 -mgeneral-regs-only`. Installation also removes compiler metadata, scopes labels, and applies the library's assembly ABI macros. Do not overwrite the production assembly blindly from a fresh compiler output.

An unrolled LZMA literal/tree trial gave no material source/binary improvement; branch-based range updates regressed those corpora. A manual x86 conditional-move trial and a zstd hash-loop unroll were also rejected. The selected source benchmark improved with branch-free zstd bit extraction and the two-byte gzip candidate filter. Sampled profiles locate the remaining zstd encode work in match/hash processing and most decode work in the sequence walker; the xz source/binary decode gap remains in the LZMA machine.

The larger history and block buffers trade address space and working memory for speed and compression: gzip reserves a 4 MiB block buffer; xz reserves a 4 MiB predecessor ring, roughly 2 MiB pending-history storage and a 1 MiB encoder dictionary; zstd reserves roughly 4 MiB history storage plus larger sequence/FSE tables. These are buffer reservations, not measured process RSS. Xz's early raw decision can trade compression on mixed-content chunks for incompressible throughput. Repeat and random encoded sizes still lose some rows despite much faster execution.

## Verification

The isolated codec revision, with unrelated shared-worktree edits excluded, passed:

- 65,024/65,024 core, guard-page, kernel-baseline and codec checks across x86_64, AArch64 and RV64.
- 462/462 standalone and tar interoperability checks on each of those three architectures.
- 12/12 codec output-error checks on each architecture, including failed writes and existing output files.
- 124/124 byte checks during the complete before/now/reference benchmark.
- 21,223/21,223 native M2 Pro checks for CRC, Huffman, inflate, range coding and the new LZMA span, including exact guard-page endings and packet-state comparison with the scalar decoder.
- 500/500 native M2 Pro sequence fixtures varying zero/nonzero bit widths, repeat offsets, packet counts and exact input/output limits.
- Assembly inventory: 288 routines, zero C bodies/objects, zero three-architecture parity gaps. The performance and specialization manifests pass.

AArch64 and RV64 Linux CLI interoperability used QEMU user mode. Their emulator wall times are deliberately excluded from performance results. Native M2 testing here establishes kernel correctness; it is not a native full-CLI performance comparison. No native RV64 timing, per-kernel lower-bound calculation, or all-input hardware-floor proof is supplied. The inventory's zero-C and parity properties do not establish that proof.

## Reproduction

From the repository root on a Linux host with both cross compilers and QEMU user emulators:

```sh
sh test/run compression_floor gzip xz zstd tar standard
```

Build each standalone shell with the repository's freestanding link and target baseline, for example:

```sh
gcc -O2 -static -nostdlib -nostartfiles -fno-stack-protector -fno-builtin \
  -march=x86-64 -w -T kit/spark.ld -Wl,-e,_start -Wl,--build-id=none \
  -Wl,--no-warn-rwx-segments -o shell-x64 programs/shell.c
python3 test/differential.py --harness compression --binary now=./shell-x64
python3 kit/codec_floor/io_checks.py ./shell-x64
XZ_OPT=-T1 ZSTD_NBTHREADS=1 python3 test/differential.py --harness compression \
  --binary before=./shell-before --binary now=./shell-x64 --bench --cpu 6 \
  --runs 5 --corpus-dir /path/to/corpora --output comparison.json
```

The corpus directory contains `src.bin`, `elf.bin`, `rep.bin` and `rnd.bin`; use the digests in [measurements.json](measurements.json) to identify this run's exact inputs. Without `--corpus-dir`, the harness creates a new reproducible source/random/repeat corpus from that checkout, which is a different experiment. Cross-architecture interoperability uses `--runner qemu-aarch64` or `--runner qemu-riscv64` with the corresponding binary. The harness refuses to report emulator timing as a benchmark.

On a native Apple Silicon Mac with Clang and Python, run:

```sh
python3 kit/codec_floor/native_lzma.py
python3 kit/codec_floor/native_zstd.py
```

These extract the current ARM assembly into an ignored `artifacts/codec-floor-native/` directory and use libc only as the host harness and I/O shim. Guard geometry uses the actual Darwin page size. They do not replace production code.
