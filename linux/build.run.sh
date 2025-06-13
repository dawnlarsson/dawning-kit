#!/bin/sh
source script/common

kernel_image=$(key kernel_image)
kernel_export=$(key kernel_export)

arch=$(key arch)
virt=$(key virt)

if [ "$arch" = "x86_64" ]; then
        args=" -enable-kvm -cpu host -smp 4 -m 4G"
fi

if [ "$arch" = "arm64" ]; then
        args=" -machine raspi4b -m 2G -smp 4"
fi

$virt $args -kernel $kernel_export -display sdl
