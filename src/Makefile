# display.c is included by core.c rather than compiled
# separately, so the C side stays one translation unit.
#
# It is a composite now rather than obj-y += core.o, because assembly
# cannot be included into a translation unit -- every .asm becomes its own
# object and they have to be listed together to end up in one module. The
# module is named moonwater, because the kernel's namespace is flat and
# that is one of the few places here that needs a name of its own.
obj-$(CONFIG_MOONWATER_CORE) += moonwater.o

moonwater-y := core.o

#
#       Assembly, for every architecture, out of one file each.
#
#       A .asm dropped in this directory is picked up here, translated to a .S
#       for whichever architecture the kernel is being configured for, and
#       assembled by the toolchain's own assembler through the kernel's
#       ordinary rule for .S files. See ./glue for the dialect.
#
#       Which architecture that is comes from the kernel's own configuration
#       rather than from the build scripts, so the answer cannot disagree with
#       what is actually being compiled. That holds for the .asm files built
#       here and nowhere else: script/glue_replace, which puts a .asm in the
#       place of a file the kernel already builds, runs before any of this and
#       has only the profile's "#> arch" to go on. The two agree because the
#       profile sets both, not because anything checks.
#
#       The pattern -- and the shape of the rule below -- is the one
#       lib/crypto/mips/Makefile uses to generate .S from perl.
#
glue-arch-$(CONFIG_X86_64) := x86_64
glue-arch-$(CONFIG_ARM64) := arm64
glue-arch-$(CONFIG_ARCH_RV64I) := riscv64

glue-asm := $(notdir $(wildcard $(src)/*.asm))

moonwater-y += $(glue-asm:.asm=.o)
targets += $(glue-asm:.asm=.S)

quiet_cmd_glue = GLUE    $@
      cmd_glue = sh $(src)/glue '$(glue-arch-y)' $< $@

$(obj)/%.S: $(src)/%.asm FORCE
	$(call if_changed,glue)
