# hexagonrpc 0.5.0-24-g40e5041 (upstream pull request #27)

The `hexagonrpcd` that serves the sensor DSP of the POCO X3 NFC, the Galaxy Tab S5e and the Xiaomi Pad 5: upstream
[hexagonrpc](https://github.com/linux-msm/hexagonrpc) at commit 598b591 (v0.5.0-19-g598b591) with the five commits of
upstream pull request [#27](https://github.com/linux-msm/hexagonrpc/pull/27), "Support for writing files", by Fady
(FadyAckad on GitHub). VibeOS changes nothing in them: they are here as `git format-patch` output, with their authorship, because the
pull request is not merged.

| Patch | What it does |
| --- | --- |
| `0001-hexagonfs-add-create-write-truncate-unlink-and-renam.patch` | hexagonfs: create, write, truncate, unlink and rename for mapped directories |
| `0002-apps_std-serve-fopen-fwrite-fsync-fremove-ftrunc-and.patch` | apps_std: serve fopen, fwrite, fsync, fremove, ftrunc and frename |
| `0003-listener-answer-unsupported-requests-and-fetch-input.patch` | listener: answer unsupported requests, fetch input buffers longer than 256 bytes |
| `0004-rpcd_builder-map-the-registry-s-parent-directory-whe.patch` | rpcd_builder: map the registry's parent directory when `DIR/sensors/persist` exists |
| `0005-hexagonrpcd-fix-descriptor-lifetimes-and-the-write-p.patch` | hexagonrpcd: descriptor lifetimes and the write paths added for the registry |

`series` lists them in order. The result is the pull request's head, commit
40e50412812adce9dddc2be9a4510ebce05eef89 (`refs/pull/27/head`).

## Rebuild

    git clone https://github.com/linux-msm/hexagonrpc.git && cd hexagonrpc
    git checkout 598b591 && git am PATCHES/hexagonrpc/0.5.0-24-g40e5041/*.patch
    # or: git fetch origin pull/27/head && git checkout 40e5041
    meson setup build --prefix=/usr/local   # the POCO's was cross-built: a meson cross file with
    ninja -C build                          # c = aarch64-linux-gnu-gcc and pkg-config = false
    DESTDIR=STAGE ninja -C build install

## Shipped in

`/usr/local/bin/hexagonrpcd`, `/usr/local/lib/libhexagonrpc.so.0.5` (and on the POCO
`/usr/local/libexec/hexagonrpc/chrecd`) in the device packages `vibeos-device-poco` 0.1.4.3, `vibeos-device-s5e`
0.1.4.3 and 0.1.5, and `vibeos-device-nabu` 0.1.5. The POCO's and the Pad 5's are the same cross build; the Tab S5e's
`hexagonrpcd` was built from the same source on the tablet (its library is byte-identical to the POCO's).

## Licence

GPL-3.0-or-later (`COPYING` and the file headers).

## How this set was checked

The source both builds were made from is the pull request's head 40e5041 with no local change (`git status` clean;
the tablet's copy is file for file the same). The POCO's build output is byte-identical to the shipped files in the
POCO and Pad 5 packages; the GNU build ID of the Tab S5e's shipped `hexagonrpcd` equals that of the tablet's build.
