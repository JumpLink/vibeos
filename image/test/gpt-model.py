"""gpt-model.py FIRST OUT [SECTOR]: a test disk (OUT, sparse) with the partition table of a phone's LUN, from FIRST,
a copy of that LUN's first sectors (as far as its partition entries at least). The disk is as long as the table's
alternate LBA says (Samsung writes no backup table there: OUT's end stays zeros). Its GUIDs, the disk's and every
partition's, are this phone's own: OUT gets others, made from the partition's number and name, and the CRCs again.
Only the protective MBR, the header and the entries are copied: whatever lies between the entries and the first
partition (Samsung's PIT, on an S20) stays zeros in OUT. SECTOR: 4096."""
import struct, sys, uuid, zlib
src, out = sys.argv[1], sys.argv[2]
ss = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
b = bytearray(open(src, 'rb').read())
h = b[ss:2 * ss]
if h[:8] != b'EFI PART':
    sys.exit('%s: no GPT header at LBA 1 (%d-byte sectors)' % (src, ss))
hsz, = struct.unpack_from('<I', h, 12)
chk = bytearray(h[:hsz]); struct.pack_into('<I', chk, 16, 0)
if struct.unpack_from('<I', h, 16)[0] != zlib.crc32(chk):
    sys.exit('header CRC')
cur, alt, first, last = struct.unpack_from('<QQQQ', h, 24)
ent_lba, n, esz = struct.unpack_from('<QII', h, 72)
eb = n * esz
ent = bytearray(b[ent_lba * ss:ent_lba * ss + eb])
if len(ent) != eb or struct.unpack_from('<I', h, 88)[0] != zlib.crc32(ent):
    sys.exit('entries CRC (or FIRST is too short)')
ns = uuid.UUID('6c1c6ea0-0000-4000-8000-766962656f73')
starts = []
for i in range(n):
    e = memoryview(ent)[i * esz:(i + 1) * esz]
    if bytes(e[:16]) == bytes(16):
        continue
    name = bytes(e[56:128]).decode('utf-16le').rstrip('\0')
    e[16:32] = uuid.uuid5(ns, '%d %s' % (i + 1, name)).bytes_le
    s, = struct.unpack_from('<Q', e, 32)
    if s:
        starts.append(s)
end_ent = (ent_lba * ss + eb + ss - 1) // ss
gap = b[end_ent * ss:min(len(b), min(starts) * ss)]
left = len(gap) - gap.count(0)
nh = bytearray(h)
nh[56:72] = uuid.uuid5(ns, 'disk').bytes_le
struct.pack_into('<I', nh, 88, zlib.crc32(ent))
struct.pack_into('<I', nh, 16, 0)
struct.pack_into('<I', nh, 16, zlib.crc32(nh[:hsz]))
mbr = b[:ss]
with open(out, 'wb') as f:
    f.truncate((alt + 1) * ss)
    f.write(mbr)
    f.write(nh)
    f.seek(ent_lba * ss); f.write(ent)
print('%s: %d sectors of %d bytes, usable %d-%d, %d partitions, GUIDs replaced%s' % (out, alt + 1, ss, first, last,
      len(starts), ', %d bytes behind the entries not copied' % left if left else ''))
