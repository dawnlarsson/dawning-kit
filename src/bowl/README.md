# Bowl

Bowl runs a distribution package on the Moonwater kernel. It is the mounts
and the exec, not a userspace and not a security boundary. POSIX tools are
Moonwater applets; a package wins by running against those, not by replacing
them.

The runtime has two profiles:

- `--fast` is the default. It creates only a private mount view, binds the
  guest loader and libc directories read-only, and executes the program by
  its path under the bowl root. Moonwater `/bin`, `/etc`, `/var`, the current
  directory, devices, process view and network stay as they are, and there is
  no supervisor fork.
- `--isolated` adds PID, UTS and IPC views, pivots into the complete
  distribution root, mounts its kernel interfaces, and supervises its first
  process. Package managers pick this themselves: `pacman`, `apt-get` and
  `apk` own the guest `/etc` and `/var`, so an exposed `pacman` is isolated
  even without the flag.

With no program, both profiles execute Moonwater's `/shell`. The runtime opens
it before changing mounts and executes that descriptor afterward, so neither a
distribution's `dash` nor its `bash` replaces the default shell. Naming
`/bin/sh` explicitly in the isolated profile still runs the distribution shell
when a maintainer script requires it.

Keep policy and setup in this directory. Shared syscall definitions remain in
`src/platform`, and the shell only supplies the multicall command entry point.
Missing POSIX tools that package managers and bootstraps call belong in the
shell as applets, not as more mounts here.

The performance rule is that steady-state work remains an ordinary native
process. Namespaces select views; they do not emulate instructions or proxy
syscalls. Setup that can be made persistent should eventually happen when a
bowl is created, not on every command invocation.

## New install

```sh
bowl setup arch
pacman -Syu
```

That is the whole first boot. Setup becomes root if it has to, points `/bowl`
at this binary so `#!/bowl` shebangs resolve, lands the official x86_64
bootstrap at `/bowls/arch`, writes a resolv.conf that is not a stub resolver,
enables one mirror, turns off pacman's alpm download sandbox (Moonwater has
no landlock), initialises the keyring inside isolated, and installs `pacman`
and `pacman-key` at `/bin` so they win on the default `PATH`. The next
command is ordinary: `pacman -Syu`, then `pacman -S jq`, then
`bowl expose /bowls/arch /usr/bin/jq` if that binary should be a fast
Moonwater command.

Setup downloads the bootstrap itself. There is no archive argument and
nothing to copy by hand.

Debian and Alpine setups are not built in yet.

`bowl expose` creates a tiny executable launcher such as `/bowls/bin/jq`, a
directory on Moonwater's default `PATH`. Keeping roots and launchers under
`/bowls` lets one persistent mount carry the complete installation. The
launcher's shebang contains the Bowl root and program path, and the kernel
invokes `/bowl` directly—there is no wrapper shell or generated per-command
binary. Ordinary exposed commands use the fast view, so `jq ./file.json` sees
the same file and working directory as a native Moonwater command, and `tar`
or `sed` in a script is still the native applet.

## Native tools the managers still need

Moonwater already has the shell, coreutils, sed, awk, grep, find, mount,
unshare, chroot, `ip`, `host`, plaintext `fetch`, `wget` (HTTPS), uncompressed
`tar`, and `zstd -d`. Pacman, apt and apk inside `--isolated` bring their own
linked downloaders and archive libraries. The host-side gaps that every distro
bootstrap still shells out to are not Bowl mounts:

- gzip / xz — Debian is `.tar.xz`, Alpine is `.tar.gz`
- `ar` — `.deb` members; debootstrap will not run without it

Do not implement pacman, apt or apk here. Do not overlay guest `/bin` to
paper over a missing applet.

The next slices should preserve these rules:

- gzip, xz, ar, then Debian and Alpine setups;
- share Arch's glibc with other glibc bowls; keep Alpine on musl;
- discover package-owned executables so exposure can be selected after install;
- persist prepared mount views and reduce fast entry to setns plus exec;
- move only measured hot operations behind a stable kernel interface.
