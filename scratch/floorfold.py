p = 'test/run'
lines = open(p).read().split('\n')

# The comment block opens at "#       Compile the whole header at the RISC-V
# floor it advertises" and the function ends at the "return 0\n}" before the
# alignment-audit comment.
start = None
for index, line in enumerate(lines):
    if line.startswith('#       Compile the whole header at the RISC-V floor'):
        start = index
        break
assert start is not None, 'floor comment not found'

end = None
for index in range(start, len(lines)):
    if lines[index].startswith('#       A result-only run under qemu cannot prove'):
        end = index
        break
assert end is not None, 'alignment comment not found'

replacement = '''#       The ISA floor, which the build tool proves and this lane reads.
#
#       `build floor` compiles the library at the floor its settings name and
#       reads the ISA attribute back out of the object, because a normal
#       toolchain defaults to something richer -- rv64gc -- and would let both
#       a compressed instruction and an extension above the floor in unnoticed.
#       It answers 2 when nothing here has that back end, which is a skip and
#       not a pass.
#
#       It lives in the tool rather than here because it is a build-time
#       guarantee about a produced object, not a comparison against a
#       reference, and because what must be present and what must be absent
#       are settings there -- so another tree gets the same proof about its own
#       floor. Which extensions this floor names and why is written beside
#       those settings.
riscv_floor()
{
        build_tool floor riscv64
}

'''.split('\n')

lines[start:end] = replacement
open(p, 'w').write('\n'.join(lines))
print("test/run: riscv_floor now calls the tool")
