# vibeos-loop.aarch64: the static helper nura-to-vibeos puts into the initramfs as bin/vibeos-loop
#   make -C image/nura/initramfs     (needs gcc-aarch64-linux-gnu and libc6-dev-arm64-cross)
CROSS_CC ?= aarch64-linux-gnu-gcc

vibeos-loop.aarch64: vibeos-loop.c
	$(CROSS_CC) -static -Os -s -o $@ $<

clean:
	rm -f vibeos-loop.aarch64

.PHONY: clean
