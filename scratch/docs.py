import pathlib

edits = {
 'kit/code_map/REGEX_PROOF.md': [
   ("Build `programs/shell.c` with `sh kit/build`, create the normal multicall name farm,",
    "Build `programs/shell.c` with `./build freestanding`, create the normal multicall name farm,")],
 'kit/code_map/SHELL_HARDWARE.md': [
   ("A normal Linux build is `sh kit/build programs/shell.c /tmp/moonwater-after`.",
    "A normal Linux build is `./build freestanding programs/shell.c /tmp/moonwater-after`.")],
 'kernel/README': [
   ("Kernel configuration, composed in order by kit/config, last request",
    "Kernel configuration, composed in order by `build config`, last request")],
 'kernel/profile/prod': [
   ("# Each was asked for as =n first and kit/verify_config reported it, which",
    "# Each was asked for as =n first and `build verify-config` reported it, which")],
 'kernel/profile/debug_none': [
   ("# DEBUG_MEMORY_INIT and core dump support. kit/verify_config now reports",
    "# DEBUG_MEMORY_INIT and core dump support. `build verify-config` now reports")],
 'test/checks.c': [
   ("source built with kit/spark and placed in the image would prove them",
    "source built with `build spark` and placed in the image would prove them"),
   ("   Build with kit/build or the shell lane's freestanding-checks runner. */",
    "   Build with `build freestanding` or the shell lane's freestanding-checks runner. */"),
   ("Build with kit/build. Compare complete-literal shortcuts with their original",
    "Build with `build freestanding`. Compare complete-literal shortcuts with their original"),
   ("        BENCH_shell_document and BENCH_tiny are built by kit/spark and run",
    "        BENCH_shell_document and BENCH_tiny are built by `build spark` and run"),
   ("//     SPARK_CPPFLAGS=-DBENCH_tiny sh kit/spark test/checks fs/tiny.spark",
    "//     SPARK_CPPFLAGS=-DBENCH_tiny ./build spark test/checks fs/tiny.spark"),
   ("//     SPARK_CPPFLAGS=-DBENCH_exec sh kit/spark test/checks fs/bench",
    "//     SPARK_CPPFLAGS=-DBENCH_exec ./build spark test/checks fs/bench"),
   ("                SPARK_CPPFLAGS=-DBENCH_network_spawn sh kit/spark test/checks \\",
    "                SPARK_CPPFLAGS=-DBENCH_network_spawn ./build spark test/checks \\")],
 '.gitignore': [
   ("# kit/asm writes a .S beside each .asm and kbuild assembles it there, because",
    "# `build asm` writes a .S beside each .asm and kbuild assembles it there, because")],
 'kit/onbox': [],
}

for path, pairs in edits.items():
    file = pathlib.Path(path)
    text = file.read_text()
    for old, new in pairs:
        if old not in text:
            print("MISS %s: %r" % (path, old[:60]))
            continue
        text = text.replace(old, new)
    file.write_text(text)
print("docs updated")
