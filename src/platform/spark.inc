/* Private entry ABI shared by the Spark loader and freestanding _start. */
#ifndef MOONWATER_SPARK_ENTRY_INCLUDED
#define MOONWATER_SPARK_ENTRY_INCLUDED

#define SPARK_START_MAGIC 0x5350524b53544132UL /* "SPRKSTA2" */
/* Version 2 changes the feature word from packed bits to Boolean byte lanes.
   Keeping that change in the magic makes either side of a mixed old/new pair
   reject the private handoff and use the ordinary CPUID fallback instead of
   silently decoding the other version's feature layout. */
/* Append-only Boolean byte lanes. Older loaders zero the complete feature
   register before setting their two known lanes; older runtimes ignore the
   extra lanes. Extending this word therefore keeps the version-2 ABI. */
#define SPARK_CPU_AVX2 0x0001UL
#define SPARK_CPU_AVX512 0x0100UL
#define SPARK_CPU_AVX512_VBMI 0x010000UL
#define SPARK_CPU_FMA 0x01000000UL

/* Final exec still applies its rlimit-dependent argument ceiling. This is
   only the device-copy ceiling, kept at Linux's ordinary ARG_MAX floor. */
#define SPARK_SPAWN_MAX_BYTES (2UL << 20)
#define SPARK_SPAWN_MAX_STRINGS 65536U

#endif
