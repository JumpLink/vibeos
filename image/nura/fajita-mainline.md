# OnePlus 6T on a newer mainline kernel

Work in progress. The 6T's zip uses Nura's kernel (7.1). This page tracks a VibeOS root on the
sdm845-mainline `v7.4-wip` branch (7.4.0-rc2), until it is good enough for its own device stage.

## Kernel

- Base: [sdm845-mainline](https://gitlab.com/sdm845-mainline/linux) `v7.4-wip`, Nura's config plus
  `INPUT_QCOM_PMI8998_HAPTICS=m` (vibration) and `VIDEO_LC89821X=m` (the focus actuators).
- Extra commits on top: the focus actuators wired up with their `vio` supply, `interconnect: qcom: sdm845:
  keep MM1 voted in the sleep set` (the display-off reset, below) and the 10nm DSI PLL digtop fix.
  Tree: [JumpLink/linux-fajita](https://github.com/JumpLink/linux-fajita) branch `fajita`.
- Test variants ran with `fastboot boot` against the installed VibeOS 0.1.3 root. They differ only in the
  command line (`module_blacklist=…`), repacked with mkbootimg: header v0, base 0, kernel offset 0x8000,
  ramdisk offset 0x1000000, tags offset 0x100, page size 4096.
- Since 2026-11-24 the kernel with these fixes lives in `boot_a` for daily use, written with `dd` from the
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
| Battery (bq27411), WLAN, sound cards | detected; battery reads 100 %, 4.385 V, full charge 2.43 Ah of 3.64 Ah design (aged cell). `energy-full-design` missing in DT (warning) |
| Modem | works with a SIM: LTE, registered, `connected`. The modem reads the card but opens no provisioning session by itself (ModemManager: `sim-missing`); `qcom-uim-provision` opens it before ModemManager starts ([#12](https://github.com/pulpoff/vibeos/pull/12)). The 6T-own firmware from `modem_a` behaves the same as the enchilada files. Do not stop/start the modem remoteproc: it sent the phone into Qualcomm CrashDump once |
| Camera focus (lc89821x) | works: the actuators share the CCI bus whose pull-ups hang on `cam_vio_1p8`, which their node did not request (`-110` while the sensor slept). With `vio-supply` (linux-fajita `175fde03c`, `c7f666693`) focus moves on both cameras with runtime PM on |
| DSI PLL warning at probe | fixed: the clock core enabled the 10nm PLL before the PHY's digital top was powered, so the lock failed and the count went unbalanced. linux-fajita `e9ac0107b` powers digtop with the PLL bias, as on 7nm; no WARN at boot, display and touch fine |
| ramoops | the region at `0xac300000` survives CrashDump intact (read with a Sahara memory dump), but not a reboot: after a cold or warm reboot the zone headers come back with bit errors or uncorrectable, so pstore is empty. Practical path: after a panic, dump the region from CrashDump ([linux-fajita#4](https://github.com/JumpLink/linux-fajita/issues/4)) |
| Audio | playback path works (tfa98xx up, no xrun); microphone works after a fresh boot (UCM HiFi Mic1 `hw:0,1`, Mic2 `hw:0,3`, PipeWire default source has signal); the zeros seen earlier came after long uptime and did not reproduce, the dmesg "no backend DAIs" lines also appear in good runs. Vibration: works (`qcom_pmi8998_haptics`, an FF_RUMBLE effect is felt). Suspend: s2idle only, untested. Plymouth: to test |
