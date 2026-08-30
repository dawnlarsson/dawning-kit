/* Private entry ABI shared by the Spark loader and freestanding _start. */
#ifndef MOONWATER_SPARK_ENTRY_INCLUDED
#define MOONWATER_SPARK_ENTRY_INCLUDED

#define SPARK_START_MAGIC 0x5350524b53544152UL /* "SPRKSTAR" */
#define SPARK_CPU_AVX2 0x01UL
#define SPARK_CPU_AVX512 0x02UL

/* Final exec still applies its rlimit-dependent argument ceiling. This is
   only the device-copy ceiling, kept at Linux's ordinary ARG_MAX floor. */
#define SPARK_SPAWN_MAX_BYTES (2UL << 20)
#define SPARK_SPAWN_MAX_STRINGS 65536U

#endif
