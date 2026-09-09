p = 'src/build/build.c'
s = open(p).read()

listed = open('scratch/part_main.c').read()
rest = open('scratch/part_rest.c').read()

anchor = """/*
        Every path in this tool is relative to the repository root, so running"""
assert anchor in s
s = s.replace(anchor, listed + "\n" + rest + "\n" + anchor, 1)

old = '''        /*      The image's own layout: the directories every build makes and'''
new = '''        /*      Booting the built image, and where the module's own build
                products land beside its source. */
        {"emulator", "qemu-system-x86_64"},
        {"emulator_flags", "-m 2G -smp 2 -cpu Nehalem"},
        {"emulator_devices",
         "-vga none -device virtio-gpu-pci -device qemu-xhci"
         " -device usb-tablet -device usb-kbd -no-reboot"},
        {"kernel_cmdline", "console=ttyS0 drm_client_lib.active="},
        {"module_root", "src"},
        {"clean_patterns",
         "[!.]*.a [!.]*.o [!.]*.o.d [!.]*.cmd [!.]*.order"
         " [!.]*.S [!.]*.asm_tmp"},

        /*      What a remote build does not need a copy of: the upstream
                kernel tree, its artifacts and the built filesystem are large
                and none of them belong to this checkout. */
        {"remote_excludes", ".git .claude linux artifacts fs dist"},

        /*      The image's own layout: the directories every build makes and'''
assert old in s
s = s.replace(old, new)
open(p, 'w').write(s)
print("ok")
