# canvas/ is included by core.c rather than compiled separately, so the C
# side stays one translation unit.
#
# It is a composite now rather than obj-y += core.o, because assembly
# cannot be included into a translation unit -- every .asm becomes its own
# object and they have to be listed together to end up in one module. The
# module is named moonwater, because the kernel's namespace is flat and
# that is one of the few places here that needs a name of its own.
obj-$(CONFIG_MOONWATER_CORE) += moonwater.o

#       A stock comparison: MOONWATER_STOCK in the environment builds the
#       module without the alias block, so the kernel keeps its own strlen,
#       memcpy and the rest and the two images can be timed against each
#       other. build.sh empties the claim and displace lists to match.
ccflags-y += $(if $(MOONWATER_STOCK),-DSTOCK_STRINGS)

moonwater-y := core.o

#
#       Assembly, for every architecture, out of one file each.
#
#       A .asm dropped in this directory is picked up here, translated to a .S
#       for whichever architecture the kernel is being configured for, and
#       assembled by the toolchain's own assembler through the kernel's
#       ordinary rule for .S files. See kit/asm for the dialect.
#
#       Two directories, named rather than found recursively. kernel/replace/
#       holds the other kind of .asm -- the ones that stand in for a file the
#       kernel already builds -- and those must not be linked into the module
#       as well, or the symbol they define exists twice and vmlinux does not
#       link. Everything that draws lives in canvas/, assembly included.
#
#       Which architecture that is comes from the kernel's own configuration
#       rather than from the build scripts, so the answer cannot disagree with
#       what is actually being compiled. That holds for the .asm files built
#       here and nowhere else: kit/asm_replace, which puts a .asm in the
#       place of a file the kernel already builds, runs before any of this and
#       has only the profile's "#> arch" to go on. The two agree because the
#       profile sets both, not because anything checks.
#
#       The pattern -- and the shape of the rule below -- is the one
#       lib/crypto/mips/Makefile uses to generate .S from perl.
#
asm-arch-$(CONFIG_X86_64) := x86_64
asm-arch-$(CONFIG_ARM64) := arm64
asm-arch-$(CONFIG_ARCH_RV64I) := riscv64

asm-sources := $(notdir $(wildcard $(src)/*.asm)) \
               $(patsubst $(src)/%,%,$(wildcard $(src)/canvas/*.asm))

moonwater-y += $(asm-sources:.asm=.o)
targets += $(asm-sources:.asm=.S)

#       $(src) is kernel/moonwater inside the kernel tree, which is a symlink
#       to this repository's src/. A relative path out of it would land back
#       in the kernel tree, so realpath follows the link home first.
quiet_cmd_asm = ASM     $@
      cmd_asm = sh $(realpath $(src))/../kit/asm '$(asm-arch-y)' $< $@

$(obj)/%.S: $(src)/%.asm FORCE
	$(call if_changed,asm)
