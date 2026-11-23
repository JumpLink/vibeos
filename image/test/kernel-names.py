#!/usr/bin/env python3
# kernel-names.py [--base DIR] FILE...: the Linux kernels in each file, wherever they sit: as they are, in gzip, xz
# or zstd streams at any offset (an Android boot image, a recovery partition, an initramfs), in an EFI zboot image's
# payload, and in a FIT image's images. Each stream found is searched again, three levels deep. Prints every
# kernel's banner with the build machine's names masked, every decompressed stream that holds one of them, and lz4
# streams it could not open. The names come from AUDIT_NAMES (colon separated: the build machine's user and host
# names). Exit 1 when a kernel's banner names one, or a stream holds "NAME@NAME" or /home/USER/.
import lzma, mmap, os, re, shutil, struct, subprocess, sys, zlib

MAXOUT = 512 << 20		# no stream is decompressed past this
CHUNK = 1 << 20
names = sorted({n for n in os.environ.get("AUDIT_NAMES", "").split(":") if n}, key=len, reverse=True)
if not names:
	sys.exit("kernel-names.py: set AUDIT_NAMES")
alt = b"|".join(re.escape(n.encode()) for n in names)
WORD = re.compile(rb"(?<![A-Za-z0-9_])(?:" + alt + rb")(?![A-Za-z0-9_])", re.I)
PAIR = re.compile(rb"(?:" + alt + rb")@(?:" + alt + rb")(?![A-Za-z0-9_])", re.I)
HOME = re.compile(rb"/home/(?:" + alt + rb")/", re.I)
BANNER = re.compile(rb"Linux version ([!-~]{1,64}) \(([^()\x00\n]{1,80})\)([^\x00\n]{0,240})")
VER = re.compile(rb"\) (#[!-~]*(?: [ -~]{0,60})?)")
PROC = re.compile(rb"%s version %s \(([^()\x00\n]{1,80})\)")
leaks = 0


def mask(b):
	return WORD.sub(b"<build-name>", b).decode("ascii", "replace")


def say(kind, chain, text):
	print(f"{kind} {chain}: {text}", flush=True)


def gunzip_at(mv, off):
	d = zlib.decompressobj(16 + zlib.MAX_WBITS)
	out, pos = [], off
	try:
		while pos < len(mv) and not d.eof:
			out.append(d.decompress(mv[pos:pos + CHUNK], MAXOUT))
			pos += CHUNK
			if sum(map(len, out)) >= MAXOUT:
				break
	except zlib.error:
		return None, 0
	data = b"".join(out)
	if not data:
		return None, 0
	end = pos - len(d.unused_data) if d.eof else pos
	return data, end


def unxz_at(mv, off, fmt=lzma.FORMAT_XZ):
	d = lzma.LZMADecompressor(fmt)
	out, pos = [], off
	try:
		while pos < len(mv) and not d.eof:
			out.append(d.decompress(mv[pos:pos + CHUNK], MAXOUT))
			pos += CHUNK
			if sum(map(len, out)) >= MAXOUT:
				break
	except lzma.LZMAError:
		return None, 0
	data = b"".join(out)
	return (data, pos - len(d.unused_data) if d.eof else pos) if data else (None, 0)


def tool(argv, data):
	if not shutil.which(argv[0]):
		return None
	p = subprocess.run(argv, input=data, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
	return p.stdout[:MAXOUT] or None		# zstd -d stops with an error at trailing bytes, after the frame


def decompress(comp, mv):
	comp = comp.lower()
	if comp in ("none", ""):
		return bytes(mv)
	if comp == "gzip":
		return gunzip_at(mv, 0)[0]
	if comp == "xz":
		return unxz_at(mv, 0)[0]
	if comp in ("lzma",):
		return unxz_at(mv, 0, lzma.FORMAT_ALONE)[0]
	if comp == "zstd":
		return tool(["zstd", "-dc"], bytes(mv))
	if comp == "lzo":
		return tool(["lzop", "-dc"], bytes(mv))
	return None


def fdt_images(b):
	# a FIT image's /images/* nodes: (name, {property: bytes}); FDT v17 structure block
	magic, total, off_struct, off_strings = struct.unpack_from(">IIII", b, 0)
	if magic != 0xd00dfeed or total > len(b):
		return []
	strings = bytes(b[off_strings:total])
	pos, path, nodes, cur = off_struct, [], [], None
	while pos + 4 <= total:
		tok, = struct.unpack_from(">I", b, pos); pos += 4
		if tok == 1:		# BEGIN_NODE
			end = bytes(b[pos:pos + 256]).index(b"\0")
			path.append(bytes(b[pos:pos + end]).decode("ascii", "replace"))
			pos += (end + 4) & ~3
			cur = {} if len(path) == 3 and path[1] == "images" else None
			if cur is not None:
				nodes.append((path[2], cur))
		elif tok == 2:		# END_NODE
			path.pop(); cur = None
		elif tok == 3:		# PROP
			ln, no = struct.unpack_from(">II", b, pos); pos += 8
			name = strings[no:strings.index(b"\0", no)].decode("ascii", "replace")
			if cur is not None and len(path) == 3:
				cur[name] = b[pos:pos + ln]
			pos += (ln + 3) & ~3
		elif tok == 4:		# NOP
			continue
		else:			# END
			break
	return nodes


def cpio_member(b, at):
	# the newc cpio member that holds offset AT of an archive starting at 0
	pos = 0
	while pos + 110 <= len(b) and bytes(b[pos:pos + 6]) in (b"070701", b"070702"):
		h = bytes(b[pos:pos + 110])
		fs, ns = int(h[54:62], 16), int(h[94:102], 16)
		name = bytes(b[pos + 110:pos + 110 + ns - 1]).decode("utf-8", "replace")
		dpos = (pos + 110 + ns + 3) & ~3
		if pos <= at < dpos + fs:
			return name
		pos = (dpos + fs + 3) & ~3
		if name == "TRAILER!!!":
			break
	return None


def look(b, chain, depth, top):
	global leaks
	mv = memoryview(b)
	ban = list(BANNER.finditer(b))
	proc = list(PROC.finditer(b))
	inside = []
	if ban and proc:
		for m in ban:
			if m.group(1) == b"%s":
				continue	# printk's format for the banner, not a banner
			who, v = m.group(2), VER.search(m.group(3))
			ver = v.group(1).rstrip() if v else b""
			bad = bool(WORD.search(who))
			leaks += bad
			say("LEAK kernel" if bad else "kernel", chain, f"{mask(m.group(1))} ({mask(who)}) {mask(ver)}")
			inside.append((m.start(), m.end()))
		for m in proc:
			bad = bool(WORD.search(m.group(1)))
			leaks += bad
			if bad:
				say("LEAK kernel", chain, f"the /proc/version format string names ({mask(m.group(1))})")
			inside.append((m.start(), m.end()))
		rest = [m for m in WORD.finditer(b) if not any(s <= m.start() < e for s, e in inside)]
		if rest:
			say("CHECK kernel", chain, f"{len(rest)} more build names in it, e.g. "
				+ " | ".join(mask(bytes(b[max(0, m.start() - 24):m.end() + 24])).replace("\n", " ") for m in rest[:3]))
	elif not top:
		pairs, homes, words = PAIR.findall(b), HOME.findall(b), WORD.findall(b)
		if pairs or homes:
			leaks += 1
			say("LEAK", chain, f"{len(pairs)} NAME@NAME and {len(homes)} /home/NAME/ in this stream")
		if words:
			where = sorted({cpio_member(b, m.start()) or "?" for m in WORD.finditer(b)})[:8]
			say("CHECK", chain, f"{len(words)} build names as words in this stream (cpio members: {', '.join(where)})")
	if depth >= 3:
		return
	done = []
	def free(o):
		return not any(s <= o < e for s, e in done)
	if bytes(mv[:2]) == b"MZ" and bytes(mv[4:8]) == b"zimg":
		off, size = struct.unpack_from("<II", mv, 8)
		comp = bytes(mv[0x18:0x20]).rstrip(b"\0").decode("ascii", "replace")
		out = decompress(comp, mv[off:off + size])
		say("stream", chain, f"EFI zboot payload ({comp}, {size} bytes at {off}): "
			+ (f"{len(out)} bytes" if out else "NOT DECOMPRESSED"))
		leaks += out is None
		if out:
			look(out, f"{chain}@zboot-{comp}", depth + 1, False)
		done.append((off, off + size))
	if bytes(mv[:4]) == b"\xd0\x0d\xfe\xed":
		for name, p in fdt_images(mv):
			if "data" not in p:
				continue
			comp = bytes(p.get("compression", b"none")).rstrip(b"\0").decode("ascii", "replace")
			out = decompress(comp, p["data"])
			kind = bytes(p.get("type", b"?")).rstrip(b"\0").decode("ascii", "replace")
			say("stream", chain, f"FIT image {name} (type {kind}, {comp}, {len(p['data'])} bytes): "
				+ (f"{len(out)} bytes" if out else "NOT DECOMPRESSED"))
			if out:
				look(out, f"{chain}@fit:{name}", depth + 1, False)
	for m in re.finditer(rb"\x1f\x8b\x08", b):
		if not free(m.start()):
			continue
		out, end = gunzip_at(mv, m.start())
		if out and len(out) > 64:
			done.append((m.start(), end))
			look(out, f"{chain}@gzip:{m.start()}", depth + 1, False)
	for m in re.finditer(rb"\xfd7zXZ\x00", b):
		if not free(m.start()):
			continue
		out, end = unxz_at(mv, m.start())
		if out:
			done.append((m.start(), end))
			look(out, f"{chain}@xz:{m.start()}", depth + 1, False)
	for m in re.finditer(rb"\x28\xb5\x2f\xfd", b):
		if not free(m.start()):
			continue
		out = tool(["zstd", "-dc"], bytes(mv[m.start():m.start() + (256 << 20)]))
		if out and len(out) > 64:
			done.append((m.start(), m.start() + 1))
			look(out, f"{chain}@zstd:{m.start()}", depth + 1, False)
	if not shutil.which("lz4"):
		n = len(re.findall(rb"\x02\x21\x4c\x18|\x04\x22\x4d\x18", b))
		if n:
			say("CHECK", chain, f"{n} lz4 stream header(s) not opened (no lz4 here)")


args = sys.argv[1:]
base = ""
if args[:1] == ["--base"]:
	base, args = args[1].rstrip("/") + "/", args[2:]
for f in args:
	if os.path.getsize(f) == 0:
		continue
	with open(f, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as mm:
		look(mm, f[len(base):] if f.startswith(base) else f, 0, True)
sys.exit(1 if leaks else 0)
