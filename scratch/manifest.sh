#!/bin/sh
#       A fingerprint of everything a build produces, for old-vs-new diffing.
cd /home/dawn/mw/proc-build || exit 1
out=$1
{
        echo "== artifacts/.config"
        sha256sum artifacts/.config artifacts/info artifacts/merge.config 2>/dev/null
        echo "== dist"
        sha256sum dist/* 2>/dev/null
        echo "== fs manifest"
        find fs -mindepth 1 -printf '%y %m %p %l\n' 2>/dev/null | sort
        echo "== fs file hashes"
        find fs -type f -printf '%p\n' 2>/dev/null | sort | xargs -r sha256sum
        echo "== generated .S"
        sha256sum src/*.S 2>/dev/null
        echo "== asm state"
        cat artifacts/asm.applied artifacts/asm.arch artifacts/asm.requested 2>/dev/null
        echo "== linux/.config"
        sha256sum linux/.config 2>/dev/null
        echo "== vmlinux/bzImage"
        sha256sum linux/arch/x86/boot/bzImage linux/vmlinux 2>/dev/null
} > "$out" 2>&1
