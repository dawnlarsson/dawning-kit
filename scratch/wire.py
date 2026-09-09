import re

# ---------------------------------------------------------------- test/run
p = 'test/run'
s = open(p).read()

old = '                sh kit/build programs/shell.c "$subject"'
new = '                build_tool freestanding programs/shell.c "$subject"'
assert s.count(old) == 4, s.count(old)
s = s.replace(old, new)

# One helper, defined once, that bootstraps the tool if it is not there yet.
anchor = "#       One section of test/checks.c, which is where every C check lives. The"
assert anchor in s
helper = '''#       The build tool, bootstrapped if this is the first thing to want it.
#
#       src/build/build.c is the whole build path -- what kit/build, kit/spark,
#       kit/asm, kit/config and kit/verify_config used to be -- and lanes reach
#       it the same way build.sh does, with the one documented command.
build_tool()
{
        if [ ! -x "$root/build" ] ||
                [ "$root/src/build/build.c" -nt "$root/build" ]
        then
                ${CC:-cc} -O2 -static -nostdlib -nostartfiles \\
                        -fno-stack-protector -fno-builtin -w \\
                        -o "$root/build" "$root/src/build/build.c" || return 1
        fi

        "$root/build" "$@"
}

'''
s = s.replace(anchor, helper + anchor, 1)
open(p, 'w').write(s)
print("test/run: 4 call sites rewired")

# ------------------------------------------------------ test/differential.py
p = 'test/differential.py'
s = open(p).read()

old = '''            subprocess.run(["sh", str(root / "kit/asm"), arch,
                            str(root / f"src/canvas/{name}.asm"), str(target)], check=True)'''
new = '''            subprocess.run([build_tool(root), "asm", arch,
                            str(root / f"src/canvas/{name}.asm"), str(target)], check=True)'''
assert old in s
s = s.replace(old, new)

old = '''        return subprocess.run(["sh", str(HARNESS_ROOT / "kit/build"), *map(str, args)],
                              cwd=self.work, env=self.env, capture_output=True,
                              text=True, timeout=10)'''
new = '''        return subprocess.run([build_tool(HARNESS_ROOT), "freestanding",
                               *map(str, args)],
                              cwd=self.work, env=self.env, capture_output=True,
                              text=True, timeout=10)'''
assert old in s
s = s.replace(old, new)

old = '''        process = subprocess.Popen(["sh", str(HARNESS_ROOT / "kit/build"), "--watch",
                                    str(source), str(self.work / "out")],'''
new = '''        process = subprocess.Popen([build_tool(HARNESS_ROOT), "freestanding",
                                    "--watch",
                                    str(source), str(self.work / "out")],'''
assert old in s
s = s.replace(old, new)

# The fixture models a remote that has already run build.sh, so it has the
# tool: that is what the image query asks.
old = '''        (remote / "kit").mkdir(parents=True)
        (remote / "kit/common").write_text((HARNESS_ROOT / "kit/common").read_text())
        (remote / "artifacts").mkdir()'''
new = '''        (remote / "kit").mkdir(parents=True)
        # A remote that has been built on has the tool, because build.sh over
        # there compiled it. The image query asks that tool, not a shell.
        tool = remote / "build"
        tool.write_text('#!/bin/sh\\n'
                        'sed -n "s|^#> $2 ||p" artifacts/.config\\n')
        tool.chmod(0o755)
        (remote / "artifacts").mkdir()'''
assert old in s
s = s.replace(old, new)

# The helper itself, beside HARNESS_ROOT.
old = "def harness_build_tools(argv):"
assert old in s
helper = '''def build_tool(root):
    """The build tool, bootstrapped once with the one documented command."""
    tool = pathlib.Path(root) / "build"
    source = pathlib.Path(root) / "src/build/build.c"

    if not tool.exists() or source.stat().st_mtime > tool.stat().st_mtime:
        subprocess.run([os.environ.get("CC_BOOTSTRAP", "cc"), "-O2", "-static",
                        "-nostdlib", "-nostartfiles", "-fno-stack-protector",
                        "-fno-builtin", "-w", "-o", str(tool), str(source)],
                       check=True)

    return str(tool)


'''
s = s.replace(old, helper + old, 1)
open(p, 'w').write(s)
print("test/differential.py: rewired")

# ------------------------------------------------------------- src/Makefile
p = 'src/Makefile'
s = open(p).read()
old = "      cmd_asm = sh $(realpath $(src))/../kit/asm '$(asm-arch-y)' $< $@"
new = "      cmd_asm = $(realpath $(src))/../build asm '$(asm-arch-y)' $< $@"
assert old in s
s = s.replace(old, new)
s = s.replace("#       ordinary rule for .S files. See kit/asm for the dialect.",
              "#       ordinary rule for .S files. See `build asm` for the dialect.")
open(p, 'w').write(s)
print("src/Makefile: rewired")

# ------------------------------------------------------ kernel/replace/apply
p = 'kernel/replace/apply'
s = open(p).read()
old = '        sh kit/asm "$arch" "$asm" "$destination"'
new = '        ./build asm "$arch" "$asm" "$destination"'
assert old in s
s = s.replace(old, new)
s = s.replace("#       builds, which is the other half of what kit/asm is for: not everything",
              "#       builds, which is the other half of what `build asm` is for: not everything")
open(p, 'w').write(s)
print("kernel/replace/apply: rewired")

# -------------------------------------------------------- kit/profile_shell
p = 'kit/profile_shell'
s = open(p).read()
old = '        (cd "$root" && sh kit/build programs/shell.c "$subject")'
new = '        (cd "$root" && ./build freestanding programs/shell.c "$subject")'
assert old in s
s = s.replace(old, new)
open(p, 'w').write(s)
print("kit/profile_shell: rewired")

# ------------------------------------------------------ kernel/patch/apply
p = 'kernel/patch/apply'
s = open(p).read()
s = s.replace("#       The spellings below are the ones kit/asm normalizes, so",
              "#       The spellings below are the ones `build asm` normalizes, so")
open(p, 'w').write(s)

# --------------------------------------------------------------- .gitignore
p = '.gitignore'
s = open(p).read()
if "\n/build\n" not in s:
    s += "\n# The build tool, compiled from src/build/build.c by build.sh.\n/build\n"
open(p, 'w').write(s)
print(".gitignore: the tool is ignored")
