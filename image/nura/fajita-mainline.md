# OnePlus 6T on a newer mainline kernel

Work in progress. The 6T's zip uses Nura's kernel (7.1). This page tracks a VibeOS root on the
sdm845-mainline `v7.4-wip` branch (7.4.0-rc2), until it is good enough for its own device stage.

## Kernel

- Base: [sdm845-mainline](https://gitlab.com/sdm845-mainline/linux) `v7.4-wip`, Nura's config plus
  `INPUT_QCOM_PMI8998_HAPTICS=m` (vibration) and `VIDEO_LC89821X=m` (the focus actuators).
- Two extra commits: `arm64: dts: qcom: sdm845-oneplus: wire up the focus actuators` and
  `interconnect: qcom: sdm845: keep MM1 voted in the sleep set` (the display-off reset, below).
- Tested with `fastboot boot` only, against the installed VibeOS 0.1.3 root; nothing is flashed. Variants
  differ only in the command line (`module_blacklist=…`), repacked with mkbootimg: header v0, base 0,
  kernel offset 0x8000, ramdisk offset 0x1000000, tags offset 0x100, page size 4096.

## How to watch a test kernel

A crash ends in Qualcomm's CrashDump mode, which leaves pstore empty and the journal without that boot.
Only a log streamed to the PC while it runs survives. On SDM845 the WLAN firmware runs on the modem DSP,
so the stream goes over the USB network (`vibeos-usb-net`, PR #7):
`ssh user@172.16.42.1 'sudo dmesg -w'` on the PC, written to a file.

## Status

| Area | 7.4.0-rc2 |
|---|---|
| Boot to the desktop, WLAN, USB network | works |
| Screen blanking (GNOME idle, DPMS) | fixed: 7.4 reset once the CPU cluster powered down with the display off, because BCM MM1 had no vote in the RPMh sleep set. With the fix 10 min display off survive |
| Modem with SIM | to test next |
| Vibration, camera, audio, battery, suspend, Bluetooth, Plymouth | to test |
