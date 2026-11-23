# OnePlus 6T on a newer mainline kernel

Work in progress. The 6T's zip uses Nura's kernel (7.1). This page tracks a VibeOS root on the
sdm845-mainline `v7.4-wip` branch (7.4.0-rc2), until it is good enough for its own device stage.

## Kernel

- Base: [sdm845-mainline](https://gitlab.com/sdm845-mainline/linux) `v7.4-wip`, Nura's config plus
  `INPUT_QCOM_PMI8998_HAPTICS=m` (vibration) and `VIDEO_LC89821X=m` (the focus actuators).
- Two extra commits: `arm64: dts: qcom: sdm845-oneplus: wire up the focus actuators` and
  `interconnect: qcom: sdm845: keep MM1 voted in the sleep set` (the display-off reset, below).
- Test variants ran with `fastboot boot` against the installed VibeOS 0.1.3 root. They differ only in the
  command line (`module_blacklist=…`), repacked with mkbootimg: header v0, base 0, kernel offset 0x8000,
  ramdisk offset 0x1000000, tags offset 0x100, page size 4096.
- Since 2026-11-24 the MM1-fixed kernel lives in `boot_a` for daily use, written with `dd` from the
  running system and read back. Never switch slots with `qbootctl -s` on the 6T: it gave `boot_b` the
  GUID of `abl_b`, and the bootloader then refused both slots until the GPT attributes were repaired over EDL.

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
| Normal reboot from `boot_a` | works, SSH after ~32 s |
| Bluetooth | works once the controller has an address: the 6T DT and bootloader give none, so the QCA chip stays "unconfigured" and BlueZ hides it. A boot service sets one derived from `androidboot.serialno` (`btmgmt public-addr`, retried until the firmware download finished) |
| Battery (bq27411), WLAN, sound cards | detected |
| Modem | visible in ModemManager; SIM and calls to test |
| Camera focus (lc89821x) | `failed to set DAC: -110` while the sensor sleeps: the actuators share the CCI bus whose pull-ups hang on `cam_vio_1p8`, which their node did not request. Held on at runtime, focus moves without error on both cameras. Fix (`vio-supply`) committed, boot test pending |
| Known warnings | DSI PLL clock disabled twice at probe; ramoops header ECC error |
| Vibration, audio playback, suspend, Plymouth | to test |
