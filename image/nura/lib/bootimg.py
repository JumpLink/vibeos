"""Android boot images: read headers v0..v4, write v0..v2 (what Nura's Qualcomm phones boot), the way mkbootimg
lays them out (pages, the SHA-1 id of v0..v2)."""
import hashlib, struct, zlib

MAGIC = b'ANDROID!'


def pages(n, ps):
    return (n + ps - 1) // ps * ps


def parse(d):
    if d[:8] != MAGIC:
        raise ValueError('not an Android boot image')
    hv = struct.unpack_from('<I', d, 40)[0]
    h = {'header_version': hv}
    if hv >= 3:
        ks, rs, osv, hs = struct.unpack_from('<4I', d, 8)
        h.update(kernel_size=ks, ramdisk_size=rs, os_version=osv, header_size=hs, page_size=4096)
        h['cmdline'] = d[44:44 + 1536].split(b'\0')[0]
        ps = 4096
        o = ps
        h['kernel'] = d[o:o + ks]; o += pages(ks, ps)
        h['ramdisk'] = d[o:o + rs]; o += pages(rs, ps)
        if hv == 4:
            h['signature_size'] = struct.unpack_from('<I', d, 1580)[0]
        h['second'] = b''; h['dtb'] = b''; h['recovery_dtbo'] = b''
        return h
    ks, ka, rs, ra, ss, sa, ta, ps, _hv, osv = struct.unpack_from('<10I', d, 8)
    h.update(kernel_size=ks, kernel_addr=ka, ramdisk_size=rs, ramdisk_addr=ra, second_size=ss, second_addr=sa,
             tags_addr=ta, page_size=ps, os_version=osv)
    h['name'] = d[48:64]
    h['cmdline'] = d[64:64 + 512].split(b'\0')[0]
    h['id'] = d[576:608]
    h['extra_cmdline'] = d[608:608 + 1024].split(b'\0')[0]
    o = ps
    h['kernel'] = d[o:o + ks]; o += pages(ks, ps)
    h['ramdisk'] = d[o:o + rs]; o += pages(rs, ps)
    h['second'] = d[o:o + ss]; o += pages(ss, ps)
    h['recovery_dtbo'] = b''; h['dtb'] = b''
    if hv >= 1:
        rds, rdo, hs = struct.unpack_from('<IQI', d, 1632)
        h.update(recovery_dtbo_size=rds, recovery_dtbo_offset=rdo, header_size=hs)
        h['recovery_dtbo'] = d[o:o + rds]; o += pages(rds, ps)
    if hv == 2:
        dbs, dba = struct.unpack_from('<IQ', d, 1648)
        h.update(dtb_size=dbs, dtb_addr=dba)
        h['dtb'] = d[o:o + dbs]; o += pages(dbs, ps)
    h['image_size'] = o
    return h


def base_and_offsets(h):
    """mkbootimg's base and offsets back from the addresses (kernel offset 0x8000 by convention when the base is
    not given; what matters to the bootloader is the addresses themselves, which build() writes unchanged)"""
    return {k: h[k] for k in ('kernel_addr', 'ramdisk_addr', 'second_addr', 'tags_addr') if k in h}


def build(h, kernel, ramdisk, cmdline, extra_cmdline=b''):
    """A v0/v1/v2 image with h's addresses, page size, name, os_version, second, recovery_dtbo and dtb, and the
    given kernel, ramdisk and command line"""
    hv, ps = h['header_version'], h['page_size']
    if hv > 2:
        raise ValueError('header v%d: writing it is not supported (v0..v2 only)' % hv)
    if len(cmdline) > 511 or len(extra_cmdline) > 1023:
        raise ValueError('command line too long for the header')
    second, rdtbo, dtb = h.get('second', b''), h.get('recovery_dtbo', b''), h.get('dtb', b'')
    hdr = bytearray(ps)
    hdr[:8] = MAGIC
    struct.pack_into('<10I', hdr, 8, len(kernel), h['kernel_addr'], len(ramdisk), h['ramdisk_addr'], len(second),
                     h['second_addr'], h['tags_addr'], ps, hv, h.get('os_version', 0))
    hdr[48:64] = (h.get('name') or b'\0' * 16)[:16].ljust(16, b'\0')
    hdr[64:64 + len(cmdline)] = cmdline
    hdr[608:608 + len(extra_cmdline)] = extra_cmdline
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        sha.update(blob); sha.update(struct.pack('<I', len(blob)))
    if hv >= 1:
        sha.update(rdtbo); sha.update(struct.pack('<I', len(rdtbo)))
    if hv >= 2:
        sha.update(dtb); sha.update(struct.pack('<I', len(dtb)))
    hdr[576:608] = sha.digest().ljust(32, b'\0')
    body = [kernel, ramdisk, second]
    if hv >= 1:
        o = ps + pages(len(kernel), ps) + pages(len(ramdisk), ps) + pages(len(second), ps)
        hsz = 1632 + 16 + (12 if hv == 2 else 0)
        struct.pack_into('<IQI', hdr, 1632, len(rdtbo), o if rdtbo else 0, hsz)
        body.append(rdtbo)
    if hv == 2:
        struct.pack_into('<IQ', hdr, 1648, len(dtb), h['dtb_addr'])
        body.append(dtb)
    out = bytearray(hdr)
    for b in body:
        out += b + b'\0' * (pages(len(b), ps) - len(b))
    return bytes(out)


# ---- the kernel inside: Image.gz (+ DTBs appended), Image, or an EFI zboot image
FDT = b'\xd0\x0d\xfe\xed'


def split_kernel(k):
    """(format, image_bytes_decompressed_or_None, compressed_len, [appended DTBs])"""
    if k[:2] == b'\x1f\x8b':
        dco = zlib.decompressobj(16 + zlib.MAX_WBITS)
        img = dco.decompress(k)
        used = len(k) - len(dco.unused_data)
        rest = k[used:]
        dtbs = []
        while len(rest) >= 8 and rest[:4] == FDT:
            n = struct.unpack('>I', rest[4:8])[0]
            dtbs.append(rest[:n]); rest = rest[n:]
        if rest.strip(b'\0'):
            raise ValueError('%d unknown bytes after the kernel' % len(rest))
        return 'Image.gz', img, used, dtbs
    if k[56:60] == b'ARM\x64':
        size = struct.unpack_from('<Q', k, 16)[0]
        rest, dtbs = k[size:], []
        # an Image's header gives its size (image_size, 0 on old kernels)
        while size and len(rest) >= 8 and rest[:4] == FDT:
            n = struct.unpack('>I', rest[4:8])[0]
            dtbs.append(rest[:n]); rest = rest[n:]
        return 'Image', k, len(k) - sum(map(len, dtbs)), dtbs
    if k[:2] == b'MZ' and k[4:8] == b'zimg':
        return 'EFI zboot', None, len(k), []
    return 'unknown', None, len(k), []


def linux_version(img):
    """'7.1.0-rc1-sdm845' from the "Linux version ..." banner of a decompressed Image"""
    i = img.find(b'Linux version ')
    if i < 0:
        return None
    return img[i + 14:img.index(b' ', i + 14)].decode()


def dtb_model(dtb):
    """(model, [compatible...]) of a flattened device tree's root node"""
    magic, total, off_struct, off_strings = struct.unpack_from('>4I', dtb, 0)
    if magic != 0xd00dfeed:
        return None, []
    o, depth, props = off_struct, 0, {}
    strings = dtb[off_strings:]
    while True:
        tok = struct.unpack_from('>I', dtb, o)[0]; o += 4
        if tok == 1:  # BEGIN_NODE
            e = dtb.index(b'\0', o); o = (e + 4) & ~3
            depth += 1
            if depth > 1:
                break
        elif tok == 3:  # PROP
            ln, nameoff = struct.unpack_from('>II', dtb, o); o += 8
            name = strings[nameoff:strings.index(b'\0', nameoff)].decode()
            props[name] = dtb[o:o + ln]; o = (o + ln + 3) & ~3
        elif tok in (2, 9):
            break
    model = props.get('model', b'').rstrip(b'\0').decode(errors='replace')
    compat = [c.decode(errors='replace') for c in props.get('compatible', b'').split(b'\0') if c]
    return model, compat
