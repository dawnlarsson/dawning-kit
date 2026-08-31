<img width="1155" height="130" alt="Dawning Moonwater Linux Header" src="https://github.com/user-attachments/assets/ade939a9-ae94-4643-adea-131ed173b5a2" />



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

## "Moonwater"?
Some believe that if you leave a bowl of water outside under a full moon, it absorbs celestial energy. 
I thought it was a funny name for this, as much of this project is unproven and experimental.

## License
Apache-2.0 license
