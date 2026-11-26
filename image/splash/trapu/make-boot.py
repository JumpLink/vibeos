#!/usr/bin/python3 -I
"""make-boot.py SVG OUT: each device's boot pictures, from the Trapu logo (trapu-logo.svg, as trapu.com has it).

  OUT/DEV/boot.png   the Plymouth picture, the panel's size as Plymouth sees it: the logo on black
  OUT/DEV/logo.png   the logo alone, for a panel of another size (trapu.script centres it)
  OUT/DEV/logo.jpg   S10e and S20: the same picture as boot.png for up_param, which the bootloader shows at
                     power-on (baseline JPEG, 4:4:4, quality 100, as Samsung's own logo.jpg), so the picture does not move or
                     change when Plymouth takes over

The logo is the SVG rendered by librsvg, nothing redrawn: its ink box centred at the place and as tall as the red
Debian swirl the bootloader drew there before (the phones: 190 px; the tablets: 297 px). Needs python3-gi
(Rsvg 2.0), python3-cairo and python3-pil.
"""
import io
import sys

import gi

gi.require_version('Rsvg', '2.0')
from gi.repository import Rsvg  # noqa: E402
import cairo  # noqa: E402
from PIL import Image  # noqa: E402

SVG, OUT = sys.argv[1], sys.argv[2]

# device: panel (as Plymouth sees it), the logo's centre, its height, whether up_param gets a logo.jpg
DEVICES = {
    's10e': ((1080, 2280), (540, 1140), 190, True),    # up_param's swirl: 152x190 centred on the panel
    's20':  ((1080, 2400), (540, 1200), 190, True),    # the same, on the 1080x2400 panel
    'poco': ((1080, 2400), (540, 1131), 190, False),   # where its U-Boot draws the swirl (152x190 at 463,1036)
    's5e':  ((2560, 1600), (1280, 800), 297, False),   # its U-Boot and init: the swirl 237x297 at 1159,653
    'nabu': ((1600, 2560), (800, 1280), 297, False),   # no swirl before it: the S5e's size, centred
}


def logo_geometry(h):
    _, w, hh = h.get_intrinsic_size_in_pixels()
    vp = Rsvg.Rectangle()
    vp.x, vp.y, vp.width, vp.height = 0, 0, w, hh
    ok, ink, _ = h.get_geometry_for_layer(None, vp)
    assert ok
    return w, hh, ink


def draw_logo(cr, cx, cy, height):
    h = Rsvg.Handle.new_from_file(SVG)
    w, hh, r = logo_geometry(h)
    s = height / r.height
    vp = Rsvg.Rectangle()
    vp.width, vp.height = w * s, hh * s
    vp.x = cx - (r.x + r.width / 2) * s
    vp.y = cy - (r.y + r.height / 2) * s
    h.render_document(cr, vp)
    return r.width * s


def to_pil(surf):
    buf = io.BytesIO()
    surf.write_to_png(buf)
    return Image.open(io.BytesIO(buf.getvalue()))


for dev, ((W, H), (cx, cy), height, jpg) in DEVICES.items():
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    cr = cairo.Context(surf)
    cr.set_source_rgb(0, 0, 0)
    cr.paint()
    lw = draw_logo(cr, cx, cy, height)
    im = to_pil(surf).convert('RGB')
    im.save(f'{OUT}/{dev}/boot.png', optimize=True)
    # the logo alone, on black, in a box of whole pixels round its ink
    bw, bh = int(lw) + 4, height + 4
    ls = cairo.ImageSurface(cairo.FORMAT_RGB24, bw, bh)
    lc = cairo.Context(ls)
    lc.set_source_rgb(0, 0, 0)
    lc.paint()
    draw_logo(lc, bw / 2, bh / 2, height)
    to_pil(ls).convert('RGB').save(f'{OUT}/{dev}/logo.png', optimize=True)
    if jpg:
        im.save(f'{OUT}/{dev}/logo.jpg', 'JPEG', quality=100, subsampling=0, progressive=False, optimize=False)
    print(f'{dev}: {W}x{H}, logo {lw:.1f}x{height} centred at ({cx},{cy}); logo.png {bw}x{bh}' + (', logo.jpg' if jpg else ''))
