<img width="1155" height="130" alt="Dawning Linux Header (1) (1)" src="https://github.com/user-attachments/assets/30e9c273-0f93-456a-ae8d-1a3010f3bc24" />

<br><br>

Moonwater is a research distro, think "Linux++"
foundational parts of the common userspace is moved into the moonwater kernel module to provide a immutable working base version of the system regardless of the userspace, performance and latency is a huge part of the why of this project.

Moonwater is also supposed to be super small, sub 15 mb for the entire system.

## Building a desktop or server image

The default build starts the in-kernel Canvas desktop. Two profiles provide
console-first images without changing the shell, utilities, Spark loader or
automatic network setup:

```sh
# Keep Canvas in the image, but start the shell directly on the kernel console.
sh build.sh arch/x64 debug_none limbo desktop serial terminal

# Do not compile or link Canvas or its architecture-specific renderer assembly.
sh build.sh arch/x64 debug_none limbo desktop serial server
```

The same split is available in Kconfig. `CONFIG_MOONWATER_CANVAS_AUTOSTART=n`
keeps Canvas compiled but leaves the display to the framebuffer, virtual or
serial console. `CONFIG_MOONWATER_CANVAS=n` removes Canvas completely. A
console-first configuration must not use the `drm_client_lib.active=` kernel
argument because that argument deliberately suppresses the framebuffer
console used in place of Canvas.

## Selecting the bundled userspace

The shell and utility payloads are independent Kconfig components. Both
default to enabled, preserving the complete image:

| `MOONWATER_SHELL` | `MOONWATER_UTILITIES` | Bundled payload |
| --- | --- | --- |
| `y` | `y` | One `/shell` image providing PID 1, `/bin/sh` and all applets |
| `y` | `n` | PID 1 and the shell, without the general utility registry |
| `n` | `y` | Utility-only multicall image, without `/init` or `/bin/sh` |
| `n` | `n` | No bundled userspace program |

The latter two configurations need another initramfs to provide `/init`.
The utility-only image stays at `/shell` because that path is part of Spark's
accelerated tool-spawn ABI, but an unknown applet returns 127 rather than
starting an interactive shell.

`fs/` is Moonwater's generated staging tree and each build clears its program
entries, so it is not an overlay input for a replacement init. A profile that
disables the shell must point `CONFIG_INITRAMFS_SOURCE` at a separate,
user-owned tree or archive, or boot with an external root filesystem.

`CONFIG_MOONWATER_UTIL_LINUX=n` removes the util-linux applet roots while
keeping the general utility surface. When the shell and utilities are enabled,
`CONFIG_MOONWATER_SHELL_MONITOR=n` omits the native monitor applet and its thin
launcher scripts. Disabled registry roots are discarded by the section linker,
so these settings reduce the compiled payload as well as the installed names.

## Bowl

Bowl runs Debian, Arch, or another Linux userspace directly on the Moonwater
kernel. The default fast profile merges the distribution runtime with the host
filesystem without a VM or supervisor fork; the system profile supplies the
complete namespace and root view needed by package managers. Selected package
executables can be exposed on Moonwater's global path with `bowl expose`.

See [the Bowl runtime notes](src/bowl/README.md) for the current commands and
installation flow.

## "Moonwater"?
Some believe that if you leave a bowl of water outside under a full moon, it absorbs celestial energy. 
I thought it was a funny name for this, as much of this project is unproven and experimental.

## License
Apache-2.0 license
