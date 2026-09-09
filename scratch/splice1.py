p = 'src/build/build.c'
s = open(p).read()
part = open('scratch/part_build.c').read()

anchor = """/*
        Every path in this tool is relative to the repository root, so running"""
assert anchor in s
s = s.replace(anchor, part + "\n" + anchor, 1)

old = '''        /*      Linking a freestanding binary of this tree's own shape. */
        {"link_script", "kit/spark.ld"},
        {"entry", "_start"},
        {"program_source", "programs/shell.c"},
'''
new = '''        /*      Linking a freestanding binary of this tree's own shape.
                The head and tail are separate so the whole-program flags land
                where they always have. Flag order does not change the output,
                but a diff of two build logs should not claim it did. */
        {"link_script", "kit/spark.ld"},
        {"entry", "_start"},
        {"freestanding_source", "src/main.c"},
        {"freestanding_output", "bin"},
        {"freestanding_flags",
         "-static -s -flto -nostdlib -nostartfiles -ffreestanding -fno-builtin"
         " -Qn -Wl,--build-id=none -Wl,--gc-sections -Wl,--strip-all"
         " -Wl,--strip-debug -Wl,-x -Wl,-s -Wl,--no-warn-rwx-segments"
         " -Wl,-nmagic -O2"},
        {"whole_program_flags", "-fwhole-program -fipa-pta"},
        {"freestanding_flags_tail",
         "-fno-asynchronous-unwind-tables -fomit-frame-pointer"
         " -fno-stack-protector -fno-semantic-interposition"
         " -D_FORTIFY_SOURCE=0 -fno-unwind-tables -fno-plt -fno-PIE -fno-pie"
         " -fno-stack-clash-protection"},
'''
assert old in s
s = s.replace(old, new)
s = s.replace('        {"floor_arch", "riscv64"},',
              '        {"floor_arch", "riscv64"},\n        {"floor_prefix", "rv64"},')
open(p, 'w').write(s)
print("ok")
