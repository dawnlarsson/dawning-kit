/* Private entry ABI shared by the Spark loader and freestanding _start. */
#ifndef MOONWATER_SPARK_ENTRY_INCLUDED
#define MOONWATER_SPARK_ENTRY_INCLUDED

#define SPARK_START_MAGIC 0x5350524b53544132UL /* "SPRKSTA2" */
/* Version 2 changes the feature word from packed bits to Boolean byte lanes.
   Keeping that change in the magic makes either side of a mixed old/new pair
   reject the private handoff and use the ordinary CPUID fallback instead of
   silently decoding the other version's feature layout. */
/* Append-only Boolean byte lanes. Older loaders zero the complete feature
   register before setting their known lanes; older runtimes ignore the
   extra lanes. Extending this word therefore keeps the version-2 ABI.

   x86_64: the carry-less multiply and AES lanes are SSE-level and need no
   saved state beyond xmm, so a loader publishes them on any processor that
   has them. VPCLMULQDQ and VAES are published only once ymm state is saved;
   a body that wants them in zmm also tests the AVX-512 lane. With these
   four the 64-bit word is full: a ninth lane needs a second register and a
   new magic.

   arm64 (x20) and riscv64 (s3) use the same byte lanes from the bottom: the
   carry-less multiply (PMULL, Zbc) in byte 0 and AES (the AES extension,
   Zvkned with usable V) in byte 1. */
#define SPARK_CPU_AVX2 0x0001UL
#define SPARK_CPU_AVX512 0x0100UL
#define SPARK_CPU_AVX512_VBMI 0x010000UL
#define SPARK_CPU_FMA 0x01000000UL
#define SPARK_CPU_PCLMUL 0x0100000000UL
#define SPARK_CPU_AES 0x010000000000UL
#define SPARK_CPU_VPCLMUL 0x01000000000000UL
#define SPARK_CPU_VAES 0x0100000000000000UL
#define SPARK_CPU_CLMUL_LANE 0x0001UL
#define SPARK_CPU_AES_LANE 0x0100UL

/* Final exec still applies its rlimit-dependent argument ceiling. This is
   only the device-copy ceiling, kept at Linux's ordinary ARG_MAX floor. */
#define SPARK_SPAWN_MAX_BYTES (2UL << 20)
#define SPARK_SPAWN_MAX_STRINGS 65536U

#endif
