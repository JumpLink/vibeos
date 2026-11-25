#!/bin/bash
# build-ubshim.sh u-boot.bin out.bin  (SHIM_CFLAGS: extra -D for the assembler)
set -eu
W=$(mktemp -d)
aarch64-linux-gnu-gcc ${SHIM_CFLAGS:-} -DUBOOT_BIN="\"$(readlink -f "$1")\"" -x assembler-with-cpp \
	-c $(dirname "$0")/r8s-ubshim.S -o $W/s.o
aarch64-linux-gnu-ld -Ttext=0 $W/s.o -o $W/s.elf
aarch64-linux-gnu-objcopy -O binary $W/s.elf "$2"
aarch64-linux-gnu-objdump -d $W/s.elf | sed -n '/<_start>:/,/<payload>:/p' | head -90 > "$2.dis"
rm -rf $W
cmp -s <(tail -c +4097 "$2") "$1" && echo "payload at +0x1000 matches $1"
ls -la "$2"
