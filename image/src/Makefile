# s10e-part, s10e-logo: static aarch64 binaries for LineageOS recovery (the
# zip ships these) and host builds for tests on the dev box.
#   make            -> s10e-part s10e-logo (aarch64, static) + *-host
CROSS_CC ?= aarch64-linux-gnu-gcc
HOST_CC  ?= gcc
CFLAGS   ?= -Os -std=gnu11 -Wall -Wextra

all: s10e-part s10e-part-host s10e-logo s10e-logo-host

# Recovery has no glibc we could count on, hence -static.
s10e-part: s10e-part.c
	$(CROSS_CC) -static $(CFLAGS) -o $@ $<
	aarch64-linux-gnu-strip $@

s10e-part-host: s10e-part.c
	$(HOST_CC) $(CFLAGS) -o $@ $<

s10e-logo: s10e-logo.c
	$(CROSS_CC) -static $(CFLAGS) -o $@ $<
	aarch64-linux-gnu-strip $@

s10e-logo-host: s10e-logo.c
	$(HOST_CC) $(CFLAGS) -o $@ $<

clean:
	rm -f s10e-part s10e-part-host s10e-logo s10e-logo-host

.PHONY: all clean
