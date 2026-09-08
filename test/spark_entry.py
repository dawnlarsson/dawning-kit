#!/usr/bin/env python3
"""Actual Spark entry publication plus exhaustive kernel capability/state gates."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
core = (root / "src/core.c").read_text()
start = core.index("static unsigned long __ro_after_init spark_cpu_features;")
features = core[start:core.index("#endif", start)]
source = r'''
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
#define __ro_after_init
#define __init
#define XCR_XFEATURE_ENABLED_MASK 0
enum { X86_FEATURE_OSXSAVE, X86_FEATURE_AVX, X86_FEATURE_AVX2,
       X86_FEATURE_AVX512F, X86_FEATURE_AVX512BW, X86_FEATURE_AVX512VL,
       X86_FEATURE_AVX512VBMI, X86_FEATURE_FMA };
static unsigned capabilities, reads;
static u64 enabled;
#define cpu_feature_enabled(feature) ((capabilities >> (feature)) & 1)
static u64 xgetbv(unsigned index) { reads++; return enabled; }
'''
source += '#include "' + str(root / "src/platform/spark.inc") + '"\n'
source += features
source += r'''
int main(void) {
    unsigned count=0;
    for (capabilities=0; capabilities<256; capabilities++)
    for (enabled=0; enabled<256; enabled++) {
        unsigned long expected=0;
        reads=0;
        spark_cpu_features=0;
        spark_cpu_features_start();
        if ((capabilities & 3) == 3 && (enabled & 6) == 6) {
            if (capabilities & 128) expected |= 0x1000000;
            if (capabilities & 4) {
                expected |= 1;
                if ((enabled & 0xe6) == 0xe6 && (capabilities & 56) == 56) {
                    expected |= 256;
                    if (capabilities & 64) expected |= 65536;
                }
            }
        }
        if (spark_cpu_features != expected || reads != ((capabilities & 3)==3))
            return 1;
        count++;
    }
    printf("spark kernel capability/state: %u of %u\n",count,count);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="moonwater-spark-entry-") as temporary:
    work = Path(temporary)
    kernel = work / "kernel.c"
    kernel.write_text(source)
    compiler = os.environ.get("CC", "gcc")
    subprocess.run([compiler, "-O2", str(kernel), "-o", str(work / "kernel")], check=True)
    subprocess.run([str(work / "kernel")], check=True)
    if os.environ.get("TEST_TALLY"):
        with open(os.environ["TEST_TALLY"], "a") as tally:
            tally.write("spark-kernel-features 65536 65536\n")
    if platform.system() == "Linux" and platform.machine() in ("x86_64", "amd64"):
        binary = work / "entry"
        subprocess.run([compiler, "-O2", "-static", "-nostdlib", "-nostartfiles",
                        "-fno-stack-protector", "-fno-builtin", "-w",
                        "-T", str(root / "kit/spark.ld"), "-Wl,-e,spark_entry_probe",
                        "-Wl,--build-id=none", "-Wl,--no-warn-rwx-segments",
                        str(root / "test/spark_entry.c"), "-o", str(binary)], check=True)
        for mode in range(9):
            subprocess.run([str(binary), str(mode)], check=True)
        print("spark old/new/fallback entry: 9 of 9")
        if os.environ.get("TEST_TALLY"):
            with open(os.environ["TEST_TALLY"], "a") as tally:
                tally.write("spark-entry 9 9\n")
    else:
        print("spark x86 entry: not run (requires native Linux x86-64)")
