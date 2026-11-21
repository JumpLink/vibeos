"""Android sparse image -> raw, streamed (the format fastboot flashes)."""
import struct

MAGIC = 0xED26FF3A


def is_sparse(head):
    return len(head) >= 4 and struct.unpack_from('<I', head)[0] == MAGIC


def unsparse(f, out):
    """f: a readable stream positioned at the sparse header; out: a seekable writable file. Holes stay holes."""
    h = f.read(28)
    magic, major, minor, fhs, chs, bs, tblk, tchunk, _crc = struct.unpack('<IHHHHIIII', h)
    if magic != MAGIC or major != 1:
        raise ValueError('not an Android sparse image')
    f.read(fhs - 28)
    pos = 0
    for _ in range(tchunk):
        ch = f.read(chs)
        ctype, _r, csz, ttl = struct.unpack_from('<HHII', ch)
        n = csz * bs
        if ctype == 0xCAC1:  # raw
            out.seek(pos)
            left = n
            while left:
                b = f.read(min(left, 1 << 20))
                if not b:
                    raise ValueError('sparse image truncated')
                out.write(b)
                left -= len(b)
        elif ctype == 0xCAC2:  # fill
            v = f.read(4)
            if v != b'\0\0\0\0':
                out.seek(pos)
                blk = v * (bs // 4)
                for _ in range(csz):
                    out.write(blk)
        elif ctype == 0xCAC3:  # don't care
            pass
        elif ctype == 0xCAC4:  # crc32
            f.read(4)
        else:
            raise ValueError('sparse chunk type %#x' % ctype)
        pos += n
    out.truncate(tblk * bs)
    return tblk * bs
