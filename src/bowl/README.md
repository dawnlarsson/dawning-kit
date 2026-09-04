# Bowl

Bowl runs another Linux distribution's userspace directly on the Moonwater
kernel. It is compatibility through a different root filesystem, not machine
emulation and not a security boundary.

The runtime has two profiles:

- `--fast` is the default. It creates only a private mount view, overlays the
  distribution's runtime directories, and directly executes the command. The
  current directory, user files, devices, process view and network remain
  Moonwater's, and there is no supervisor fork.
- `--system` adds PID, UTS and IPC views, pivots into the complete distribution
  root, mounts its kernel interfaces, and supervises its first process. Use it
  for package managers and services that expect to own a complete system.

With no program, both profiles execute Moonwater's `/shell`. The runtime opens
it before changing mounts and executes that descriptor afterward, so neither a
distribution's `dash` nor its `bash` replaces the default shell. Naming
`/bin/sh` explicitly still runs the distribution shell when compatibility
requires it.

Keep policy and setup in this directory. Shared syscall definitions remain in
`src/platform`, and the shell only supplies the multicall command entry point.
That boundary leaves room for image lifecycle, selectable isolation profiles,
and Moonwater-native command shims without putting those policies in the shell.

The performance rule is that steady-state work remains an ordinary native
process. Namespaces select views; they do not emulate instructions or proxy
syscalls. Setup that can be made persistent should eventually happen when a
bowl is created, not on every command invocation.

## First system-wide command

Place an unpacked root at `/bowls/debian` or `/bowls/arch`, install a package in
the complete profile, then expose one of its executables:

```sh
bowl --system /bowls/debian /usr/bin/apt-get install -y jq
bowl expose /bowls/debian /usr/bin/jq
jq --version
```

The Arch equivalent is:

```sh
bowl --system /bowls/arch /usr/bin/pacman -S --noconfirm jq
bowl expose /bowls/arch /usr/bin/jq
```

`bowl expose` creates a tiny executable launcher such as `/bowls/bin/jq`, a
directory on Moonwater's default `PATH`. Keeping roots and launchers under
`/bowls` lets one persistent mount carry the complete installation. The
launcher's shebang contains the Bowl root and program path, and the kernel
invokes `/bowl` directly—there is no wrapper shell or generated per-command
binary. Exposed commands use the fast merged view, so `jq ./file.json` sees the
same file and working directory as a native Moonwater command.

The next runtime slices should preserve these rules:

- add root acquisition and verification for Debian and Arch;
- discover package-owned executables so exposure can be selected after install;
- persist prepared mount views and reduce fast entry to setns plus exec;
- move only measured hot operations behind a stable kernel interface.
