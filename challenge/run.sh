#!/bin/sh

qemu-system-x86_64 -cpu Broadwell \
	-m 64 \
	-kernel ./kernel \
	-nographic \
	-append "console=ttyS0 quiet" -initrd ./initramfs \
	-monitor /dev/null
