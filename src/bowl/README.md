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
  process. Package managers pick this themselves: `pacman`, `apt-get`,
  `apk`, `dnf` and `nix` own the guest `/etc` and `/var`, so an exposed
  `pacman` is isolated even without the flag.

Both profiles keep the caller's user identity and capabilities. The isolated
profile also shares the host network and has no syscall filter. Run only
trusted packages: neither profile contains hostile code, and setup executes
distribution tools as root. Keep `/bowls` and the installed launchers under
the administrator's control. Bootstrap downloads use HTTPS; setup does not
verify a detached distribution signature.

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

That is the whole first boot. Every named setup shares that pipeline: become
root if it has to, point `/bowl` at this binary so `#!/bowl` shebangs
resolve, download if the marker is missing, land (extract by compression
magic, flatten a prefix directory when the marker sits under one child),
write a resolv.conf that is not a stub resolver, apply the manager conf that
tree actually contains, and install the manager at `/bin`. Distros differ by
URL, size floor, marker, decoder, and a small prime step (Arch keyring).
There is no archive argument and nothing to copy by hand.

```sh
bowl setup alpine    # minirootfs .tar.gz, native gzip + tar
bowl setup debian    # rootfs.tar.gz, native gzip + tar
bowl setup fedora    # refuses: OCI layers, not a rootfs tarball
bowl setup nix       # refuses: a /nix store, not a distro root
```

`bowl setup arch` lands the official x86_64 bootstrap at `/bowls/arch`,
enables one mirror, turns off pacman's alpm download sandbox (Moonwater has
no landlock), initialises the keyring inside isolated, and puts `pacman` and
`pacman-key` on `PATH`. The next command is ordinary: `pacman -Syu`, then
`pacman -S jq`, then `bowl expose /bowls/arch /usr/bin/jq` if that binary
should be a fast Moonwater command.

Alpine is the minirootfs tarball (`/sbin/apk`, no prefix). Debian is
debuerreotype's official `rootfs.tar.gz` (`/usr/bin/apt-get`, no prefix).
Both land the same way as Arch: download, `tar` reads the gzip magic,
flatten-by-marker. Guest apt/apk still use their own libraries
inside isolated. Fedora's published image is an OCI archive (index,
manifest, then a layer tar), so flatten-by-marker cannot find `/usr/bin/dnf`.
Nix's binary tarball flattens to `bin/nix` plus `store/`, but those binaries
want `/nix/store`, not `/store` at a bowl root.

glibc bowls can share Arch's loader later. Alpine stays musl.

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
unshare, chroot, `ip`, `host`, plaintext `fetch`, `wget` (HTTPS), `tar`
with in-process gzip/xz/zstd, and the gzip/xz/zstd applets. Pacman, apt and apk inside `--isolated` bring their own
linked downloaders and archive libraries. The host-side gaps that every distro
bootstrap still shells out to are not Bowl mounts:

- `ar` — `.deb` members; debootstrap will not run without it
- an OCI unwrap — Fedora's container image is layers, not a rootfs tarball
- a `/nix` store bind — Nix is not a distro root

Do not implement pacman, apt or apk here. Do not overlay guest `/bin` to
paper over a missing applet.

The next slices should preserve these rules:

- ar, then remaining Debian debootstrap pieces;
- share Arch's glibc with other glibc bowls; keep Alpine on musl;
- discover package-owned executables so exposure can be selected after install;
- persist prepared mount views and reduce fast entry to setns plus exec;
- move only measured hot operations behind a stable kernel interface.
