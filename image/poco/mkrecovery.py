#!/usr/bin/env python3
"""mkrecovery.py LINEAGE.img U-BOOT.bin OUT.img AVBTOOL.py [KEY.pem]: U-Boot in one of LineageOS's own boot images
for the POCO X3 NFC: LINEAGE.img is LineageOS's recovery.img (for the RECOVERY partition, every install) or its
boot.img (for the BOOT partition, the zip's "whole phone" choice). Which one it is comes from LINEAGE.img's AVB
footer (its partition name).

LineageOS keeps boot, dtbo and vbmeta beside Android; U-Boot takes the recovery partition, so "Restart to recovery"
(or Vol+ & Power) starts Linux and a plain power-on starts Android, as on the S10e. On the whole phone U-Boot takes
boot as well, written with LineageOS's dtbo and vbmeta from the same build, so a plain power-on starts Linux too.

ABL applies a dtbo overlay to the image's DTB and stops in fastboot when that overlay does not apply (the S5e refused
every overlay but LineageOS's own; the 2026-10-13 testers landed in fastboot over a DT that did not take MIUI's
dtbo). A recovery image (header v1 or later) brings its own overlay, recovery_dtbo; a boot image takes the dtbo
PARTITION's. So the DTB, and the recovery_dtbo, stay LineageOS's byte for byte, and for boot the dtbo partition
gets LineageOS's dtbo.img of the same build (install.sh). U-Boot does not use that DT: u-boot.bin carries its own
(OF_SEPARATE, CONFIG_DEFAULT_DEVICE_TREE=qcom/sm7150-xiaomi-surya-huaxing), which board_fdt_blob_setup() prefers.
From ABL's DT it takes only the memory map and /chosen/bootargs (the panel ABL detected).

Kernel = gzip(u-boot.bin), as LineageOS's own kernel is a gzip'd Image (ABL inflates it). The Image header's
image_size is raised to cover the whole file, built-in DT included, in case ABL places anything right after
what the header claims. No ramdisk (the boot-partition U-Boot images that ran on these phones had none) and
no command line: ABL adds its own to the DT it passes. Everything else in the header is LineageOS's.

The partition ends in an AVB hash footer like LineageOS's. Xiaomi's ABL reads a recovery image's vbmeta from its
own footer (vbmeta has no descriptor for recovery) and stops on a recovery without one: "The system has been
destroyed" (2026-11-09; the Tab S5e's ABL booted one without). LineageOS signs its surya recovery with AOSP's public
test key (external/avb test/data/testkey_rsa4096.pem, public key sha1 2597c218...), so KEY is that key, checked
against LineageOS's footer. LineageOS's boot.img footer is unsigned (Algorithm NONE; vbmeta carries boot's hash,
and LineageOS's surya vbmeta has flags 3, verification off): KEY is not needed for it. AVBTOOL is AOSP's avbtool.py
(external/avb, android-15.0.0_r1). Algorithm, rollback index, properties and the salt are copied from LineageOS's
footer, so the same inputs always give the same image."""
import gzip, hashlib, os, re, struct, subprocess, sys

if len(sys.argv) not in (5, 6):
    sys.exit(__doc__.split("\n\n")[0])
src, ub, out, avbtool = sys.argv[1:5]
key = sys.argv[5] if len(sys.argv) == 6 else None


def avb(*args):
    return subprocess.run([sys.executable, "-I", avbtool, *args], check=True, capture_output=True, text=True).stdout


info = avb("info_image", "--image", src)
field = lambda name: re.search(rf"^\s*{re.escape(name)}:\s*(.*)$", info, re.M).group(1).strip()
lin_alg, lin_rb = field("Algorithm"), field("Rollback Index")
part, salt = field("Partition Name"), field("Salt")
assert part in ("recovery", "boot"), f"{src}: an AVB footer for '{part}', not recovery or boot"
props = re.findall(r"^\s*Prop: (\S+) -> '(.*)'$", info, re.M)
sign = []
if lin_alg != "NONE":
    assert key, f"{src} is signed ({lin_alg}): KEY.pem is needed"
    lin_pk = field("Public key (sha1)")
    avb("extract_public_key", "--key", key, "--output", out + ".pk")
    pk = hashlib.sha1(open(out + ".pk", "rb").read()).hexdigest()
    os.unlink(out + ".pk")
    assert pk == lin_pk, f"{key} is not the key LineageOS's {part} is signed with ({pk} != {lin_pk})"
    sign = ["--key", key]
else:
    pk = "none"

d = open(src, "rb").read()
assert d[:8] == b"ANDROID!", "not a boot image"
ks, kaddr, rs, raddr, ss, saddr, taddr, page, hv, osv = struct.unpack_from("<10I", d, 8)
assert hv == 2 and page == 4096, f"expected LineageOS's v2/4096 {part} image, got v{hv}/{page}"
dtbo_size, dtbo_off, hdr_size = struct.unpack_from("<IQI", d, 1632)
dtb_size, dtb_addr = struct.unpack_from("<IQ", d, 1648)
pad = lambda n: (n + page - 1) // page * page
o = page + pad(ks) + pad(rs) + pad(ss)
if part == "recovery":
    assert dtbo_size and o == dtbo_off and d[o:o + 4] == b"\xd7\xb7\xab\x1e", "recovery_dtbo not where the header says"
else:
    assert dtbo_size == 0 and dtbo_off == 0, "a boot image with a recovery_dtbo"
dtbo = d[o:o + dtbo_size]
o += pad(dtbo_size)
assert d[o:o + 4] == b"\xd0\x0d\xfe\xed", "no DTB where the header says"
dtb = d[o:o + dtb_size]

u = bytearray(open(ub, "rb").read())
assert u[0x38:0x3c] == b"ARMd", "u-boot.bin has no arm64 Image header (CONFIG_LINUX_KERNEL_IMAGE_HEADER)"
assert u.find(b"\xd0\x0d\xfe\xed", 0x40) > 0, "u-boot.bin carries no DT (OF_SEPARATE)"
size = struct.unpack_from("<Q", u, 16)[0]
want = (len(u) + 0xFFFF) // 0x10000 * 0x10000 + 0x100000    # the file, rounded, plus 1 MiB for the BSS
if size < want:
    struct.pack_into("<Q", u, 16, want)
kernel = gzip.compress(bytes(u), 9, mtime=0)
ramdisk = b""

# the SHA-1 id over kernel, ramdisk, second, recovery_dtbo, dtb (with sizes), as mkbootimg makes it
h = hashlib.sha1()
for blob in (kernel, ramdisk, b"", dtbo, dtb):
    h.update(blob); h.update(struct.pack("<I", len(blob)))
sha = h.digest() + b"\0" * 12

new_dtbo_off = page + pad(len(kernel)) + pad(len(ramdisk)) if dtbo else 0
hdr = bytearray(d[:hdr_size])
struct.pack_into("<10I", hdr, 8, len(kernel), kaddr, len(ramdisk), raddr, 0, saddr, taddr, page, hv, osv)
hdr[64:576] = b"\0" * 512           # no cmdline
hdr[576:608] = sha
hdr[608:1632] = b"\0" * 1024        # no extra cmdline
struct.pack_into("<IQI", hdr, 1632, len(dtbo), new_dtbo_off, hdr_size)
struct.pack_into("<IQ", hdr, 1648, len(dtb), dtb_addr)

img = bytearray()
for blob in (bytes(hdr), kernel, ramdisk, dtbo, dtb):
    img += blob + b"\0" * ((-len(blob)) % page)
assert len(img) <= len(d), f"{len(img)} bytes > the {len(d)}-byte partition"
# the image, then avbtool fills the partition: zeros, the vbmeta right after the image, the footer at the end
used = len(img)
open(out, "wb").write(bytes(img))
cmd = ["add_hash_footer", "--image", out, "--partition_size", str(len(d)), "--partition_name", part,
       "--algorithm", lin_alg, "--rollback_index", lin_rb, "--salt", salt] + sign
for k, v in props:
    cmd += ["--prop", f"{k}:{v}"]
avb(*cmd)
img = open(out, "rb").read()
assert len(img) == len(d), "avbtool did not fill the partition"
print(f"{part}: image {used} bytes + AVB footer ({lin_alg}, rollback index {lin_rb}, key {pk[:8]}), {len(d)} in all")
print(f"{out}: {len(img)} bytes, kernel {len(kernel)} (u-boot.bin {len(u)}, image_size {size} -> "
      f"{struct.unpack_from('<Q', u, 16)[0]}), recovery_dtbo {len(dtbo)}, dtb {len(dtb)}")
