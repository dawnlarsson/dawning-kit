# dawning_display.c is included by dawning_core.c rather than compiled
# separately, so the C side stays one translation unit.
#
# It is a composite now rather than obj-y += dawning_core.o, because assembly
# cannot be included into a translation unit -- every .asm becomes its own
# object and they have to be listed together to end up in one module. The
# name of that module changes from dawning_core to dawning; nothing outside
# this directory ever referred to it.
obj-$(CONFIG_DAWNING_CORE) += dawning.o

dawning-y := dawning_core.o

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
#       what is actually being compiled. The pattern -- and the shape of the
#       rule below -- is the one lib/crypto/mips/Makefile uses to generate .S
#       from perl.
#
glue-arch-$(CONFIG_X86_64) := x86_64
glue-arch-$(CONFIG_ARM64) := arm64
glue-arch-$(CONFIG_ARCH_RV64I) := riscv64

glue-asm := $(notdir $(wildcard $(src)/*.asm))

dawning-y += $(glue-asm:.asm=.o)
targets += $(glue-asm:.asm=.S)

quiet_cmd_glue = GLUE    $@
      cmd_glue = sh $(src)/glue '$(glue-arch-y)' $< $@

$(obj)/%.S: $(src)/%.asm FORCE
	$(call if_changed,glue)
