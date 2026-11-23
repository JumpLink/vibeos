"""GPT (512- or 4096-byte sectors) and MBR partition tables of a disk image, read only."""
import struct, uuid

TYPES = {
    'c12a7328-f81f-11d2-ba4b-00a0c93ec93b': 'esp',
    'bc13c2ff-59e6-4262-a352-b275fd6f7172': 'xbootldr',
    '0fc63daf-8483-4772-8e79-3d69d8477de4': 'linux',
    'b921b045-1df0-41c3-af44-4c6f280d3fae': 'root-arm64',
    'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7': 'basic-data',
}


def partitions(path):
    """[(index, name, type, first_byte, size_bytes, sector_size)]"""
    with open(path, 'rb') as f:
        for ss in (512, 4096):
            f.seek(ss)
            h = f.read(92)
            if h[:8] != b'EFI PART':
                continue
            lba, n, esz = struct.unpack_from('<QII', h, 72)
            f.seek(lba * ss)
            ent = f.read(n * esz)
            out = []
            for i in range(n):
                e = ent[i * esz:(i + 1) * esz]
                if e[:16] == b'\0' * 16:
                    continue
                t = str(uuid.UUID(bytes_le=e[:16]))
                first, last = struct.unpack_from('<QQ', e, 32)
                name = e[56:128].decode('utf-16-le').rstrip('\0')
                out.append((i + 1, name, TYPES.get(t, t), first * ss, (last - first + 1) * ss, ss))
            return out
        f.seek(0)
        m = f.read(512)
        if m[510:512] != b'\x55\xaa':
            return []
        out = []
        for i in range(4):
            e = m[446 + 16 * i:462 + 16 * i]
            t, s, c = e[4], struct.unpack_from('<I', e, 8)[0], struct.unpack_from('<I', e, 12)[0]
            if t and c and t != 0xee:
                out.append((i + 1, '', 'mbr-%02x' % t, s * 512, c * 512, 512))
        return out


def fstype(path, off):
    with open(path, 'rb') as f:
        f.seek(off)
        b = f.read(65536 + 4096)
    if b[1080:1082] == b'\x53\xef':
        label = b[1024 + 120:1024 + 136].rstrip(b'\0').decode(errors='replace')
        return 'ext4', label
    if b[0x10040:0x10048] == b'_BHRfS_M':
        return 'btrfs', b[0x1012b:0x1012b + 256].split(b'\0')[0].decode(errors='replace')
    if b[82:87] == b'FAT32' or b[54:59] in (b'FAT16', b'FAT12'):
        lab = (b[71:82] if b[82:87] == b'FAT32' else b[43:54]).decode(errors='replace').strip()
        return 'vfat', lab
    if b[1024:1028] == b'\x10\x20\xf5\xf2':
        return 'f2fs', ''
    return 'unknown', ''
