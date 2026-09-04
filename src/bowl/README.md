# Bowl

Bowl runs another Linux distribution's userspace directly on the Moonwater
kernel. It is compatibility through a different root filesystem, not machine
emulation and not a security boundary.

The first implementation deliberately has a small contract:

- enter a distribution root with native Linux namespaces and `pivot_root`;
- mount the kernel interfaces expected by normal distribution packages;
- share networking and credentials with Moonwater;
- execute the distribution's current `/bin/sh` or one explicitly named program.

Keep policy and setup in this directory. Shared syscall definitions remain in
`src/platform`, and the shell only supplies the multicall command entry point.
That boundary leaves room for image lifecycle, selectable isolation profiles,
and Moonwater-native command shims without putting those policies in the shell.

The performance rule is that steady-state work remains an ordinary native
process. Namespaces select views; they do not emulate instructions or proxy
syscalls. Setup that can be made persistent should eventually happen when a
bowl is created, not on every command invocation.

The next runtime slices should preserve that rule:

- make the default shell Moonwater's `/shell`, retained across `pivot_root` by
  file descriptor, with the distribution shell available only when requested;
- add an explicit fast profile (mount view only) beside the current system
  profile (mount, PID, UTS, and IPC views);
- build a small, opt-in shim map from distribution command names to the
  Moonwater multicall binary and its optimized routines;
- split one-time root preparation from the hot enter-and-spawn path, then move
  only measured hot operations behind a stable kernel interface.
