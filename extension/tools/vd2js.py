#!/usr/bin/env python3
# vd2js.py - Android VectorDrawable / AnimatedVectorDrawable XML -> GJS icon-data module (icondata.js).
#
#   python3 tools/vd2js.py [-o icondata.js] [--fwb DIR]
#
# Reads frameworks/base (default $FWB, else /media/pulp/VP5/S10e/gnome-phone/ref/fwb). Files missing from
# that sparse checkout are read with `git show HEAD:<path>`. The module goes to stdout or -o FILE; notes
# (wrappers followed, approximations, icons not found) go to stderr. Python 3 stdlib only.
#
# Output: export const ICONS = {name: {w, h, dw, dh, ops}}. w/h = viewport, dw/dh = size in dp. ops run in
# order, in viewport coordinates:
#   ['push'] / ['pop']                                   save/restore around each <group> that draws
#   ['clip', path, m]                                    intersect the clip (non-zero)
#   ['fill', path, alpha, evenOdd, m]                    tint colour at alpha
#   ['stroke', path, alpha, width, cap, join, miter, m]  width in path coordinates (before m)
# path = flat absolute ['M',x,y, 'L',x,y, 'C',x1,y1,x2,y2,x,y, 'Z']; m = [xx, yx, xy, yy, x0, y0]
# (cairo order) of the enclosing group transforms, or null. <animated-vector>s are emitted in the END
# state of their animators; trim paths are applied to the geometry (Skia semantics: first contour only).
#
# Semantics follow hwui: PathParser.cpp, utils/VectorDrawableUtils.cpp, VectorDrawable.cpp.

import argparse
import math
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

A = '{http://schemas.android.com/apk/res/android}'
AAPT = '{http://schemas.android.com/aapt}'

FWB = os.environ.get('FWB', '/media/pulp/VP5/S10e/gnome-phone/ref/fwb')
FLAG = 'flag(com.android.systemui.icon_refresh_2025)'
SYSUI, SLIB, CORE = 'packages/SystemUI/res', 'packages/SettingsLib/res', 'core/res/res'
FLAG_DRAWABLES = f'{SYSUI}/{FLAG}/drawable'

NOTES = []          # (icon, text), printed to stderr and summarised in the report


def note(icon, text):
    NOTES.append((icon, text))


# ---- source access: working tree first, then git ---------------------------------------------------

_text_cache = {}


def read_text(rel):
    """Contents of fwb/<rel>, or None."""
    if rel not in _text_cache:
        path = os.path.join(FWB, rel)
        if os.path.isfile(path):
            with open(path, encoding='utf-8') as f:
                _text_cache[rel] = f.read()
        else:
            r = subprocess.run(['git', '-C', FWB, 'show', f'HEAD:{rel}'], capture_output=True)
            _text_cache[rel] = r.stdout.decode('utf-8') if r.returncode == 0 else None
    return _text_cache[rel]


def list_dir(rel):
    """File names in fwb/<rel> (working tree and git tree merged)."""
    names = set()
    path = os.path.join(FWB, rel)
    if os.path.isdir(path):
        names.update(os.listdir(path))
    r = subprocess.run(['git', '-C', FWB, 'ls-tree', '--name-only', f'HEAD:{rel}'], capture_output=True)
    if r.returncode == 0:
        names.update(r.stdout.decode().split())
    return sorted(names)


def flag_dirs(res):
    return [f'{res}/{d}' for d in list_dir(res) if d.startswith('flag(')]


# ---- resources -----------------------------------------------------------------------------------------
# A file's "origin" is the res root it came from. SettingsLib is merged into SystemUI, whose resources
# (with the ON flag dir first) override it; core is only reachable through @android:.

def search_roots(origin, pkg):
    if pkg == 'android' or origin == CORE:
        return [CORE]
    return flag_dirs(SYSUI) + [SYSUI] + flag_dirs(SLIB) + [SLIB]


def parse_ref(ref):
    """'@[*][pkg:]type/name' -> (pkg, type, name) or None."""
    m = re.match(r'^@\+?\*?(?:([\w.]+):)?(\w+)/(.+)$', ref.strip())
    return m.groups() if m else None


def find_file(kind, name, origin, pkg=None):
    """(rel path, origin root) of res/<kind>/<name>.xml in lookup order."""
    for root in search_roots(origin, pkg):
        rel = f'{root}/{kind}/{name}.xml'
        if read_text(rel) is not None:
            return rel, (root if not root.split('/')[-1].startswith('flag(') else root.rsplit('/', 1)[0])
    return None, None


_values = {}


def values_index(root):
    """{(type, name): text} of <root>/values/*.xml."""
    if root not in _values:
        idx = {}
        for fn in list_dir(f'{root}/values'):
            if not fn.endswith('.xml'):
                continue
            try:
                tree = ET.fromstring(read_text(f'{root}/values/{fn}'))
            except ET.ParseError:
                continue
            for el in tree:
                typ = el.get('type') if el.tag == 'item' else el.tag
                if typ and el.get('name'):
                    idx.setdefault((typ, el.get('name')), ''.join(el.itertext()))
        _values[root] = idx
    return _values[root]


def resolve_value(val, origin, depth=0):
    """Follow @string/@dimen/... references to a literal string (aapt whitespace rules)."""
    if val is None or not val.startswith('@') or depth > 8:
        return val
    ref = parse_ref(val)
    if not ref or ref[1] in ('drawable', 'animator', 'anim', 'color'):
        return val
    pkg, typ, name = ref
    for root in search_roots(origin, pkg):
        txt = values_index(root).get((typ, name))
        if txt is not None:
            txt = re.sub(r'\s+', ' ', txt).strip().strip('"')
            return resolve_value(txt, CORE if root == CORE else origin, depth + 1)
    raise LookupError(f'unresolved {val}')


def num(val, origin, default=None):
    """Float from an attribute: literal, '24dp', or a @dimen/@integer reference."""
    if val is None:
        return default
    val = resolve_value(val, origin)
    m = re.match(r'^\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*(dp|dip|px|sp|%)?\s*$', val)
    if not m:
        raise ValueError(f'not a number: {val}')
    return float(m.group(1)) / (100 if m.group(2) == '%' else 1)


def color_alpha(val):
    """Alpha (0..1) a colour contributes. References and theme attrs are opaque tint, per the brief."""
    if val is None:
        return 0.0
    v = val.strip()
    if v in ('@android:color/transparent', '@*android:color/transparent'):
        return 0.0
    if v.startswith('#'):
        h = v[1:]
        if len(h) == 4:
            return int(h[0], 16) / 15
        if len(h) == 8:
            return int(h[:2], 16) / 255
    return 1.0


def child_attr(el, name):
    """<aapt:attr name="android:NAME"> inline child, or None."""
    for ch in el:
        if ch.tag == AAPT + 'attr' and ch.get('name') == f'android:{name}':
            return next(iter(ch), None)
    return None


# ---- path data -----------------------------------------------------------------------------------------

ARGS = {'m': 2, 'l': 2, 't': 2, 'h': 1, 'v': 1, 'c': 6, 's': 4, 'q': 4, 'a': 7, 'z': 0}
NUMBER = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def tokenize(d):
    """[(verb, [floats])] as PathParser.cpp splits it: any letter but e/E starts a verb."""
    out = []
    for m in re.finditer(r'([A-DF-Za-df-z])([^A-DF-Za-df-z]*)', d):
        verb, nums = m.group(1), [float(x) for x in NUMBER.findall(m.group(2))]
        n = ARGS.get(verb.lower())
        if n is None or (n == 0 and nums) or (n and len(nums) % n):
            raise ValueError(f'bad path verb {verb} with {len(nums)} numbers')
        out.append((verb, nums))
    return out


def arc_to_cubics(x1, y1, rx, ry, phi, large, sweep, x2, y2):
    """SVG endpoint arc -> [('C', ...)] (F.6.5 centre parameterisation, <= 90 degrees per piece)."""
    if (x1, y1) == (x2, y2) or rx == 0 or ry == 0:
        return [('L', x2, y2)]          # Skia: identical endpoints or a zero radius -> lineTo
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry)
    if lam > 1:                         # radii too small: scale up
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num_ = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    k = math.sqrt(max(0.0, num_ / den)) * (-1 if large == sweep else 1)
    cxp, cyp = k * rx * y1p / ry, -k * ry * x1p / rx
    cx, cy = cp * cxp - sp * cyp + (x1 + x2) / 2, sp * cxp + cp * cyp + (y1 + y2) / 2
    ux, uy = (x1p - cxp) / rx, (y1p - cyp) / ry
    vx, vy = (-x1p - cxp) / rx, (-y1p - cyp) / ry
    t1 = math.atan2(uy, ux)
    dt = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(1, math.ceil(abs(dt) / (math.pi / 2) - 1e-9))
    step = dt / n
    t = 4 / 3 * math.tan(step / 4)
    pt = lambda ex, ey: (cx + rx * ex * cp - ry * ey * sp, cy + rx * ex * sp + ry * ey * cp)
    out, a = [], t1
    for i in range(n):
        b = a + step
        c1 = pt(math.cos(a) - t * math.sin(a), math.sin(a) + t * math.cos(a))
        c2 = pt(math.cos(b) + t * math.sin(b), math.sin(b) - t * math.cos(b))
        end = (x2, y2) if i == n - 1 else pt(math.cos(b), math.sin(b))
        out.append(('C', *c1, *c2, *end))
        a = b
    return out


def normalise(d):
    """Path string -> [('M',x,y) | ('L',x,y) | ('C',x1,y1,x2,y2,x,y) | ('Z',)], all absolute."""
    out = []
    cx = cy = sx = sy = 0.0             # current point, subpath start
    kx = ky = 0.0                       # last control point (hwui's ctrlPoint)
    open_ = False                       # is there a subpath to draw into?
    prev = 'm'

    def seg(s):
        nonlocal open_
        if not open_:                   # Skia injects a moveTo; after Z hwui moves to the start
            out.append(('M', sx, sy))
            open_ = True
        out.append(s)

    for verb, nums in tokenize(d):
        n, rel = ARGS[verb.lower()], verb.islower()
        if n == 0:
            out.append(('Z',))
            cx, cy = kx, ky = sx, sy
            open_ = False
        for i in range(0, len(nums), n or 1):
            p = nums[i:i + n]
            ox, oy = (cx, cy) if rel else (0.0, 0.0)
            v = verb.upper()
            if v == 'M' and i == 0:
                cx, cy = sx, sy = ox + p[0], oy + p[1]
                out.append(('M', cx, cy))
                open_ = True
            elif v in 'ML':             # extra pairs after M are L
                cx, cy = ox + p[0], oy + p[1]
                seg(('L', cx, cy))
            elif v == 'H':
                cx = ox + p[0]
                seg(('L', cx, cy))
            elif v == 'V':
                cy = oy + p[0]
                seg(('L', cx, cy))
            elif v in 'CS':
                if v == 'C':
                    x1, y1, p = ox + p[0], oy + p[1], p[2:]
                elif prev in 'cCsS':
                    x1, y1 = 2 * cx - kx, 2 * cy - ky
                else:
                    x1, y1 = cx, cy
                kx, ky = ox + p[0], oy + p[1]
                ex, ey = ox + p[2], oy + p[3]
                seg(('C', x1, y1, kx, ky, ex, ey))
                cx, cy = ex, ey
            elif v in 'QT':
                if v == 'Q':
                    qx, qy, p = ox + p[0], oy + p[1], p[2:]
                elif prev in 'qQtT':
                    qx, qy = 2 * cx - kx, 2 * cy - ky
                else:
                    qx, qy = cx, cy
                ex, ey = ox + p[0], oy + p[1]
                seg(('C', cx + 2 / 3 * (qx - cx), cy + 2 / 3 * (qy - cy),
                     ex + 2 / 3 * (qx - ex), ey + 2 / 3 * (qy - ey), ex, ey))
                kx, ky, cx, cy = qx, qy, ex, ey
            elif v == 'A':
                ex, ey = ox + p[5], oy + p[6]
                for s in arc_to_cubics(cx, cy, p[0], p[1], p[2], p[3] != 0, p[4] != 0, ex, ey):
                    seg(s)
                cx, cy = kx, ky = ex, ey
            prev = verb
        prev = verb
    while out and out[-1][0] == 'M':    # a trailing moveTo draws nothing
        out.pop()
    return out


def end_point(d):
    """Last point of a path (for a motion-path translateXY animator)."""
    segs = [s for s in normalise(d) if s[0] != 'Z']
    return segs[-1][-2:]


# ---- trim (SkContourMeasure on the first non-empty contour) ---------------------------------------------

def _bez(p, t):
    """Point on segment p = (x0,y0, ...) at t: line (4 numbers) or cubic (8)."""
    if len(p) == 4:
        return p[0] + (p[2] - p[0]) * t, p[1] + (p[3] - p[1]) * t
    u = 1 - t
    a, b, c, d = u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t
    return (a * p[0] + b * p[2] + c * p[4] + d * p[6], a * p[1] + b * p[3] + c * p[5] + d * p[7])


def _sub_cubic(p, t0, t1):
    """Control points of cubic p restricted to [t0, t1]."""
    def split(q, t):                    # de Casteljau, first half
        x01, y01 = q[0] + (q[2] - q[0]) * t, q[1] + (q[3] - q[1]) * t
        x12, y12 = q[2] + (q[4] - q[2]) * t, q[3] + (q[5] - q[3]) * t
        x23, y23 = q[4] + (q[6] - q[4]) * t, q[5] + (q[7] - q[5]) * t
        xa, ya = x01 + (x12 - x01) * t, y01 + (y12 - y01) * t
        xb, yb = x12 + (x23 - x12) * t, y12 + (y23 - y12) * t
        xm, ym = xa + (xb - xa) * t, ya + (yb - ya) * t
        return (q[0], q[1], x01, y01, xa, ya, xm, ym), (xm, ym, xb, yb, x23, y23, q[6], q[7])
    head = split(p, t1)[0] if t1 < 1 else p
    return split(head, t0 / t1)[1] if t0 > 0 and t1 > 0 else head


def _contours(path):
    """[(segments, closed)] with each segment as a point tuple; zero-length contours dropped."""
    out, segs, start, cur, closed = [], [], None, None, False
    def flush():
        if segs and sum(_seg_len(s) for s in segs) > 0:
            out.append((list(segs), closed))
    for s in path:
        if s[0] == 'M':
            flush()
            segs, start, cur, closed = [], s[1:], s[1:], False
        elif s[0] == 'Z':
            if cur != start:
                segs.append((*cur, *start))
            closed = True
            cur = start
        else:
            segs.append((*cur, *s[1:]))
            cur = s[-2:]
    flush()
    return out


def _seg_len(p, n=64):
    if len(p) == 4:
        return math.hypot(p[2] - p[0], p[3] - p[1])
    pts = [_bez(p, i / n) for i in range(n + 1)]
    return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:]))


def _t_at(p, dist, n=64):
    """Parameter t where segment p reaches arc length dist (piecewise-linear, as Skia does)."""
    if len(p) == 4:
        L = _seg_len(p)
        return dist / L if L else 0.0
    pts = [_bez(p, i / n) for i in range(n + 1)]
    acc = 0.0
    for i in range(n):
        d = math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
        if acc + d >= dist:
            return (i + (dist - acc) / d) / n if d else i / n
        acc += d
    return 1.0


def _piece(p, t0, t1):
    if len(p) == 4:
        return ('L', *_bez(p, t1))
    return ('C', *_sub_cubic(p, t0, t1)[2:])


def trim(path, start, end, offset):
    """hwui applyTrim(): trimPathStart/End/Offset, on the first contour only."""
    if start == 0 and end == 1:
        return path
    if start == end:
        return []
    cs = _contours(path)
    if not cs:
        return []
    segs = cs[0][0]
    lens = [_seg_len(s) for s in segs]
    total = sum(lens)
    a, b = total * math.fmod(start + offset, 1.0), total * math.fmod(end + offset, 1.0)

    def segment(d0, d1):
        d0, d1 = max(d0, 0.0), min(d1, total)
        if not d0 <= d1:
            return []
        def locate(d, start):           # a start on a joint belongs to the next segment
            acc = 0.0
            for i, L in enumerate(lens):
                if (d < acc + L if start else d <= acc + L) or i == len(lens) - 1:
                    return i, _t_at(segs[i], d - acc) if L else 0.0
                acc += L
        (i0, t0), (i1, t1) = locate(d0, True), locate(d1, False)
        if i0 > i1:
            i0, t0 = i1, t1
        out = [('M', *_bez(segs[i0], t0))]
        if i0 == i1:
            out.append(_piece(segs[i0], t0, t1) if t0 != t1 else ('L', *out[0][1:]))
        else:
            out.append(_piece(segs[i0], t0, 1.0))
            out += [_piece(segs[i], 0.0, 1.0) for i in range(i0 + 1, i1)]
            out.append(_piece(segs[i1], 0.0, t1))
        return out

    if a > b:
        return segment(a, total) + (segment(0, b) if b > 0 else [])
    return segment(a, b)


# ---- vector tree -----------------------------------------------------------------------------------------

class Node:
    """<vector>/<group>/<path>/<clip-path>; p holds the (animatable) properties."""
    def __init__(self, kind, name, p, origin):
        self.kind, self.name, self.p, self.origin, self.kids = kind, name, p, origin, []


GROUP_PROPS = ('rotation', 'pivotX', 'pivotY', 'scaleX', 'scaleY', 'translateX', 'translateY', 'alpha')
PATH_PROPS = ('pathData', 'fillColor', 'strokeColor', 'fillAlpha', 'strokeAlpha', 'strokeWidth',
              'trimPathStart', 'trimPathEnd', 'trimPathOffset')
GROUP_DEFAULTS = dict(rotation=0, pivotX=0, pivotY=0, scaleX=1, scaleY=1, translateX=0, translateY=0, alpha=1)


def parse_vector(el, origin, icon):
    """<vector> element -> root Node with w, h, dw, dh."""
    root = Node('vector', el.get(A + 'name'), {'alpha': num(el.get(A + 'alpha'), origin, 1.0)}, origin)
    root.w, root.h = num(el.get(A + 'viewportWidth'), origin), num(el.get(A + 'viewportHeight'), origin)
    root.dw, root.dh = num(el.get(A + 'width'), origin), num(el.get(A + 'height'), origin)

    def build(parent, xml):
        for ch in xml:
            tag = ch.tag
            if tag == 'group':
                p = {k: num(ch.get(A + k), origin, GROUP_DEFAULTS[k]) for k in GROUP_PROPS}
                if ch.get(A + 'alpha') is not None:
                    note(icon, 'non-standard <group android:alpha> honoured')
                node = Node('group', ch.get(A + 'name'), p, origin)
                build(node, ch)
            elif tag == 'path':
                fill, stroke = ch.get(A + 'fillColor'), ch.get(A + 'strokeColor')
                for attr in ('fillColor', 'strokeColor'):
                    g = child_attr(ch, attr)
                    if g is not None:
                        note(icon, f'{g.tag} {attr} drawn as flat tint')
                        if attr == 'fillColor':
                            fill = '#ffffffff'
                        else:
                            stroke = '#ffffffff'
                p = dict(pathData=ch.get(A + 'pathData'), fillColor=color_alpha(fill),
                         strokeColor=color_alpha(stroke),
                         fillAlpha=num(ch.get(A + 'fillAlpha'), origin, 1.0),
                         strokeAlpha=num(ch.get(A + 'strokeAlpha'), origin, 1.0),
                         strokeWidth=num(ch.get(A + 'strokeWidth'), origin, 0.0),
                         trimPathStart=num(ch.get(A + 'trimPathStart'), origin, 0.0),
                         trimPathEnd=num(ch.get(A + 'trimPathEnd'), origin, 1.0),
                         trimPathOffset=num(ch.get(A + 'trimPathOffset'), origin, 0.0),
                         cap=(ch.get(A + 'strokeLineCap') or 'butt').lower(),
                         join=(ch.get(A + 'strokeLineJoin') or 'miter').lower(),
                         miter=num(ch.get(A + 'strokeMiterLimit'), origin, 4.0),
                         evenOdd=(ch.get(A + 'fillType') or '').lower() in ('evenodd', '1'))
                node = Node('path', ch.get(A + 'name'), p, origin)
            elif tag == 'clip-path':
                node = Node('clip', ch.get(A + 'name'), {'pathData': ch.get(A + 'pathData')}, origin)
            elif tag == AAPT + 'attr':
                continue
            else:
                note(icon, f'unsupported <{tag}> skipped')
                continue
            parent.kids.append(node)
            if tag != 'group' and any(c.tag != AAPT + 'attr' for c in ch):
                # hwui's pull parser only pops on </group>: anything nested in a <clip-path> or <path>
                # is a following sibling in the same group (core's ic_signal_cellular_* rely on it)
                build(parent, ch)

    build(root, el)
    return root


def walk(root):
    yield root
    for k in root.kids:
        yield from walk(k)


# ---- animated-vector end state ---------------------------------------------------------------------------

def animator_values(el, t0, origin, out, icon, seq):
    """Collect (end time, order, value) per property from an animator tree; returns its end time."""
    tag = el.tag
    if tag == 'set':
        together = (el.get(A + 'ordering') or 'together') == 'together'
        t = end = t0
        for ch in el:
            if ch.tag in ('set', 'objectAnimator', 'animator'):
                e = animator_values(ch, t0 if together else t, origin, out, icon, seq)
                t, end = e, max(end, e)
        return end
    dur = num(el.get(A + 'duration'), origin, 300.0)
    off = num(el.get(A + 'startOffset'), origin, 0.0)
    rc = el.get(A + 'repeatCount') or '0'
    if rc in ('infinite', '-1'):
        note(icon, f'infinite {el.get(A + "propertyName") or "animator"} loop: static value kept')
        return t0 + off + dur
    rc = int(num(rc, origin))
    end = t0 + off + dur * (rc + 1)
    backwards = el.get(A + 'repeatMode') == 'reverse' and rc % 2 == 1

    def put(prop, value):
        seq[0] += 1
        out.setdefault(prop, []).append((end, seq[0], value))

    if el.get(A + 'pathData') and el.get(A + 'propertyXName'):     # motion path: ends at its last point
        x, y = end_point(resolve_value(el.get(A + 'pathData'), origin))
        put(el.get(A + 'propertyXName'), x)
        put(el.get(A + 'propertyYName'), y)
    elif el.get(A + 'propertyName'):
        v = el.get(A + ('valueFrom' if backwards else 'valueTo'))
        if v is not None:
            put(el.get(A + 'propertyName'), (v, origin))
    for pvh in el.iter('propertyValuesHolder'):
        frames = [k for k in pvh if k.tag == 'keyframe']
        if frames:
            fr = [num(k.get(A + 'fraction'), origin, i / max(1, len(frames) - 1)) for i, k in enumerate(frames)]
            k = frames[fr.index(min(fr) if backwards else max(fr))]
            v = k.get(A + 'value')
        else:
            v = pvh.get(A + ('valueFrom' if backwards else 'valueTo'))
        if v is not None:
            put(pvh.get(A + 'propertyName'), (v, origin))
    return end


def load_animator(target, origin, icon):
    """The target's animator element and the origin to resolve its references against."""
    inline = child_attr(target, 'animation')
    if inline is not None:
        return inline, origin
    ref = parse_ref(target.get(A + 'animation') or '')
    if not ref:
        return None, origin
    rel, aorigin = find_file(ref[1], ref[2], origin, ref[0])
    if not rel:
        note(icon, f'animator {target.get(A + "animation")} not found')
        return None, origin
    return ET.fromstring(read_text(rel)), aorigin


def apply_end_state(avd, vec, origin, icon):
    """Set every target's properties to the value of the animator that finishes last."""
    nodes = {}
    for n in walk(vec):                  # hwui's target map: a later node of the same name wins
        if n.name:
            nodes[n.name] = n
    for target in avd.findall('target'):
        name = target.get(A + 'name')
        node = nodes.get(name)
        anim, aorigin = load_animator(target, origin, icon)
        if node is None or anim is None:
            note(icon, f'target {name} has no node/animator')
            continue
        props = {}
        animator_values(anim, 0.0, aorigin, props, icon, [0])
        for prop, vals in props.items():
            value = max(vals)[2]
            allowed = PATH_PROPS if node.kind == 'path' else ('pathData',) if node.kind == 'clip' else \
                ('alpha',) if node.kind == 'vector' else GROUP_PROPS
            if prop not in allowed:
                note(icon, f'property {prop} on {node.kind} {name} ignored')
                continue
            if isinstance(value, tuple):
                v, vorigin = value
                if prop == 'pathData':
                    value = resolve_value(v, vorigin)
                elif prop in ('fillColor', 'strokeColor'):
                    value = color_alpha(resolve_value(v, vorigin))
                else:
                    value = num(v, vorigin)
            if prop in ('trimPathStart', 'trimPathEnd', 'trimPathOffset'):
                note(icon, f'{prop} animated on {name}: end state trimmed into the geometry')
            node.p[prop] = value


# ---- flatten to ops --------------------------------------------------------------------------------------

IDENT = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def mul(P, Q):
    """P after Q, both (xx, yx, xy, yy, x0, y0)."""
    return (P[0] * Q[0] + P[2] * Q[1], P[1] * Q[0] + P[3] * Q[1],
            P[0] * Q[2] + P[2] * Q[3], P[1] * Q[2] + P[3] * Q[3],
            P[0] * Q[4] + P[2] * Q[5] + P[4], P[1] * Q[4] + P[3] * Q[5] + P[5])


def group_matrix(p):
    """hwui Group::getLocalMatrix: T(-pivot), scale, rotate, T(translate + pivot)."""
    c, s = math.cos(math.radians(p['rotation'])), math.sin(math.radians(p['rotation']))
    m = (1, 0, 0, 1, -p['pivotX'], -p['pivotY'])
    m = mul((p['scaleX'], 0, 0, p['scaleY'], 0, 0), m)
    m = mul((c, s, -s, c, 0, 0), m)
    return mul((1, 0, 0, 1, p['translateX'] + p['pivotX'], p['translateY'] + p['pivotY']), m)


def flat(path):
    return [x for s in path for x in s]


def flatten(node, m, alpha, ops, icon):
    for k in node.kids:
        p = k.p
        mm = None if all(abs(a - b) < 1e-9 for a, b in zip(m, IDENT)) else m
        if k.kind == 'group':
            sub = []
            flatten(k, mul(m, group_matrix(p)), alpha * p['alpha'], sub, icon)
            if any(op[0] in ('fill', 'stroke') for op in sub):
                ops += [['push']] + sub + [['pop']]
        elif k.kind == 'clip':
            d = resolve_value(p['pathData'], k.origin)
            ops.append(['clip', flat(normalise(d)) if d else [], mm])
        elif p['pathData']:
            path = normalise(resolve_value(p['pathData'], k.origin))
            if (p['trimPathStart'], p['trimPathEnd']) != (0, 1):
                path = trim(path, p['trimPathStart'], p['trimPathEnd'], p['trimPathOffset'])
            if not path:
                continue
            fa = p['fillAlpha'] * p['fillColor'] * alpha
            if fa > 0:
                ops.append(['fill', flat(path), fa, p['evenOdd'], mm])
            sa = p['strokeAlpha'] * p['strokeColor'] * alpha
            if sa > 0 and p['strokeWidth'] > 0:
                ops.append(['stroke', flat(path), sa, p['strokeWidth'], p['cap'], p['join'], p['miter'], mm])


# ---- drawable loading ------------------------------------------------------------------------------------

def load_drawable(rel, origin, icon, chain):
    """Vector root Node for a drawable file, following wrappers. chain collects file names for the comment."""
    chain.append(rel.rsplit('/', 1)[-1] if chain else rel)
    el = ET.fromstring(read_text(rel))
    tag = el.tag
    if tag == 'vector':
        return parse_vector(el, origin, icon)
    if tag == 'animated-vector':
        inner = child_attr(el, 'drawable')
        if inner is not None:
            vec = parse_vector(inner, origin, icon)
        else:
            vec = follow(el.get(A + 'drawable'), origin, icon, chain)
        apply_end_state(el, vec, origin, icon)
        return vec
    if tag == 'inset':
        vec = follow(el.get(A + 'drawable'), origin, icon, chain, el)
        dp, fr = {}, {}                 # each side is dp or a fraction of the outer bounds
        for side in ('Left', 'Top', 'Right', 'Bottom'):
            v = resolve_value(el.get(A + 'inset' + side) or el.get(A + 'inset') or '0', origin).strip()
            pct = v.endswith('%')
            dp[side], fr[side] = (0.0, num(v, origin)) if pct else (num(v, origin), 0.0)
        if any(dp.values()) or any(fr.values()):
            # InsetDrawable.getIntrinsicWidth(): child / (1 - fractions) + dp insets
            ow = vec.dw / (1 - fr['Left'] - fr['Right']) + dp['Left'] + dp['Right']
            oh = vec.dh / (1 - fr['Top'] - fr['Bottom']) + dp['Top'] + dp['Bottom']
            left, top = dp['Left'] + fr['Left'] * ow, dp['Top'] + fr['Top'] * oh
            kx, ky = vec.w / vec.dw, vec.h / vec.dh          # dp -> viewport units
            vec.inset = (left * kx, top * ky)
            vec.w, vec.h, vec.dw, vec.dh = ow * kx, oh * ky, ow, oh
            note(icon, f'<inset> followed; box grown to {ow:g}x{oh:g} dp, drawing moved by {left:g},{top:g} dp')
        return vec
    if tag in ('layer-list', 'selector', 'animated-selector', 'level-list'):
        items = [i for i in el if i.tag == 'item']
        plain = [i for i in items if not any(k.startswith(A + 'state_') for k in i.attrib)]
        item = (plain or items)[0]
        if tag == 'layer-list' and len(items) > 1:
            note(icon, f'<layer-list> of {len(items)}: only the first item drawn')
        note(icon, f'<{tag}> followed to {item.get(A + "drawable")}')
        return follow(item.get(A + 'drawable'), origin, icon, chain, item)
    raise ValueError(f'unsupported drawable <{tag}>')


def follow(ref, origin, icon, chain, el=None):
    if ref is None and el is not None:
        for ch in el:
            if ch.tag == 'vector':
                return parse_vector(ch, origin, icon)
    r = parse_ref(ref or '')
    rel, org = find_file('drawable', r[2], origin, r[0]) if r else (None, None)
    if not rel:
        raise LookupError(f'drawable {ref} not found')
    return load_drawable(rel, org, icon, chain)


# ---- Compose ImageVector (Kotlin) -----------------------------------------------------------------------

KT_CALLS = {'moveTo': 'M', 'moveToRelative': 'm', 'lineTo': 'L', 'lineToRelative': 'l',
            'horizontalLineTo': 'H', 'horizontalLineToRelative': 'h', 'verticalLineTo': 'V',
            'verticalLineToRelative': 'v', 'curveTo': 'C', 'curveToRelative': 'c',
            'reflectiveCurveTo': 'S', 'reflectiveCurveToRelative': 's', 'quadTo': 'Q',
            'quadToRelative': 'q', 'reflectiveQuadTo': 'T', 'reflectiveQuadToRelative': 't',
            'arcTo': 'A', 'arcToRelative': 'a', 'close': 'Z'}


def load_kotlin(rel, icon):
    """A Compose ImageVector.Builder icon (path { } blocks only) -> vector root Node."""
    src = read_text(rel)
    kv = lambda k: float(re.search(rf'{k}\s*=\s*([\d.]+)', src).group(1))
    root = Node('vector', None, {'alpha': 1.0}, SYSUI)
    root.w, root.h, root.dw, root.dh = kv('viewportWidth'), kv('viewportHeight'), kv('defaultWidth'), kv('defaultHeight')
    if re.search(r'\bgroup\s*\(', src):
        note(icon, 'Compose group() not supported')
    for m in re.finditer(r'\bpath\s*\(([^{]*)\)\s*\{([^}]*)\}', src):
        args, body = m.group(1), m.group(2)
        d = []
        for call, params in re.findall(r'(\w+)\s*\(([^)]*)\)', body):
            vals = [{'true': '1', 'false': '0'}.get(x.strip(), x.strip().rstrip('f')) for x in params.split(',') if x.strip()]
            d.append(KT_CALLS[call] + ' ' + ' '.join(vals))
        def color(key):
            c = re.search(rf'{key}\s*=\s*([^,\n]*)', args)
            if not c or c.group(1).strip() == 'null':
                return 0.0
            h = re.search(r'0x([0-9A-Fa-f]{8})', c.group(1))
            return int(h.group(1)[:2], 16) / 255 if h else 1.0
        arg = lambda pat, default: (re.search(pat, args) or [None, default])[1]
        p = dict(pathData=' '.join(d), fillColor=color(r'\bfill'), strokeColor=color(r'\bstroke'),
                 fillAlpha=float(arg(r'fillAlpha\s*=\s*([\d.]+)', 1)),
                 strokeAlpha=float(arg(r'strokeAlpha\s*=\s*([\d.]+)', 1)),
                 strokeWidth=float(arg(r'strokeLineWidth\s*=\s*([\d.]+)', 0)),
                 trimPathStart=0.0, trimPathEnd=1.0, trimPathOffset=0.0,
                 cap=arg(r'StrokeCap\.(\w+)', 'butt').lower(), join=arg(r'StrokeJoin\.(\w+)', 'miter').lower(),
                 miter=float(arg(r'strokeLineMiter\s*=\s*([\d.]+)', 4)), evenOdd='EvenOdd' in args)
        root.kids.append(Node('path', None, p, SYSUI))
    return root


# ---- the icon list ---------------------------------------------------------------------------------------

SYSUI_ICONS = [
    'vd_bluetooth', 'ic_qs_footer_info', 'ic_qs_footer_power', 'ic_qs_footer_settings',
    'notif_footer_btn_history', 'notif_footer_btn_settings', 'notif_footer_btn_clear_all',
    'ic_volume_media', 'ic_volume_media_mute', 'ic_volume_ringer', 'ic_volume_ringer_mute',
    'ic_volume_ringer_vibrate', 'ic_volume_odi_captions', 'ic_volume_odi_captions_disabled',
    'ic_volume_media_bt', 'ic_volume_headset',
    'ic_brightness_medium', 'ic_brightness_low', 'ic_brightness_full',
    'ic_qs_brightness_auto_on', 'ic_qs_brightness_auto_off',
    'ic_qs_dnd_on', 'qs_dnd_icon_off', 'qs_dnd_icon_on', 'stat_sys_dnd', 'stat_sys_alarm',
    'ic_settings', 'ic_qs_no_sim',
    # QS internet / mobile data tile (InternetTileDataInteractor, MobileDataTileDataInteractor, SignalDrawable)
    'ic_qs_no_internet_available', 'ic_qs_no_internet_unavailable', 'ic_signal_mobile_data_off',
    'ic_signal_strength_zero_bar_no_internet',
]
SLIB_ICONS = ([f'ic_wifi_{i}' for i in range(4)] + [f'ic_wifi_{i}_error' for i in range(4)] +
              [f'ic_mobile_{i}_4_bar' for i in range(5)] + [f'ic_mobile_{i}_4_bar_error' for i in range(5)])
CORE_ICONS = [
    'ic_mode_edit',                                     # pencil (pre-2025 edit icon)
    'ic_chevron_end',                                   # FooterActions text-pill chevron
    'ic_notification_2025_expand', 'ic_notification_2025_collapse',
    'ic_expand_notification', 'ic_collapse_notification',
    'stat_notify_call_mute', 'stat_notify_disabled_data', 'stat_notify_wifi_in_range',
    'ic_lock_power_off', 'ic_restart',
] + [f'ic_signal_cellular_{i}_4_bar' for i in range(5)]
# Compose ImageVectors that have no XML drawable: key -> source
KOTLIN_ICONS = {'qs_edit': 'packages/SystemUI/src/com/android/systemui/common/ui/icons/Edit.kt'}


def icon_names():
    names = [n[:-4] for n in list_dir(FLAG_DRAWABLES) if n.endswith('.xml')]
    for n in SYSUI_ICONS + SLIB_ICONS + CORE_ICONS:
        if n not in names:
            names.append(n)
    return names


def locate(name):
    """Top-level lookup: flag dir, SystemUI, SettingsLib, core."""
    for root, origin in ((f'{SYSUI}/{FLAG}', SYSUI), (SYSUI, SYSUI), (SLIB, SLIB), (CORE, CORE)):
        rel = f'{root}/drawable/{name}.xml'
        if read_text(rel) is not None:
            return rel, origin
    return None, None


# ---- output ----------------------------------------------------------------------------------------------

def js_num(v):
    v = round(float(v), 4)
    if v == 0:
        return '0'
    s = f'{v:.4f}'.rstrip('0').rstrip('.')
    return s


def js(v):
    if v is None:
        return 'null'
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, (int, float)):
        return js_num(v)
    if isinstance(v, str):
        return "'" + v + "'"
    return '[' + ', '.join(js(x) for x in v) + ']'


def convert(name):
    """(source comment, vector Node, ops) for one icon, or None."""
    chain = []
    if name in KOTLIN_ICONS:
        rel = KOTLIN_ICONS[name]
        vec, chain = load_kotlin(rel, name), [rel]
    else:
        rel, origin = locate(name)
        if not rel:
            return None
        vec = load_drawable(rel, origin, name, chain)
    root = getattr(vec, 'inset', (0.0, 0.0))
    ops = []
    flatten(vec, (1, 0, 0, 1, *root), vec.p['alpha'], ops, name)
    if not ops:
        note(name, 'draws nothing')
    return ' -> '.join(chain), vec, ops


def main():
    global FWB
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('-o', '--output', help='write here instead of stdout')
    ap.add_argument('--fwb', default=FWB, help='frameworks/base checkout')
    args = ap.parse_args()
    FWB = args.fwb

    lines = ['// Generated by tools/vd2js.py from Android 16 (LineageOS 23.2) vector drawables. Do not edit.',
             f'// Source: frameworks/base ({FLAG} on); each entry names its file(s).',
             'export const ICONS = {']
    missing = []
    for name in icon_names() + list(KOTLIN_ICONS):
        try:
            res = convert(name)
        except (LookupError, ValueError, ET.ParseError) as e:
            note(name, f'FAILED: {e}')
            continue
        if res is None:
            missing.append(name)
            continue
        src, vec, ops = res
        lines.append(f'    // {src}')
        lines.append(f"    '{name}': {{w: {js(vec.w)}, h: {js(vec.h)}, dw: {js(vec.dw)}, dh: {js(vec.dh)}, ops: [")
        lines += [f'        {js(op)},' for op in ops]
        lines.append('    ]},')
    lines.append('};')
    text = '\n'.join(lines) + '\n'
    if args.output:
        with open(args.output, 'w', encoding='utf-8') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    for icon, msg in dict.fromkeys(NOTES):          # unique, in order
        print(f'{icon}: {msg}', file=sys.stderr)
    for name in missing:
        print(f'{name}: not found', file=sys.stderr)


if __name__ == '__main__':
    main()
