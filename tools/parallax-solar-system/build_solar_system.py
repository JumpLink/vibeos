#!/usr/bin/python3 -I
# build_solar_system.py DOWNLOADS OUTDIR: the "solar-system" parallax set for Neo, from NASA images (public domain)
# and procedural art (the star field, the glows, the orbits; drawn here, no third-party material).
#
# Neo draws every layer as the 2340x2340 square scaled to cover (screen + 2 margin), margin = range*|depth|*scale,
# centred (parallax.c). So the composition is laid out in screen pixels of a 1080x2400 phone at scale 3 and each
# object is mapped into its layer's square with that layer's zoom.
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

Image.MAX_IMAGE_PIXELS = None
SRC, OUT = sys.argv[1], sys.argv[2]
N = 2340                        # layer size
SW, SH, SCALE = 1080, 2400, 3   # the screen the layout is drawn for
RANGE, TILT = 60, 0.2
DEPTHS = [-0.6, -0.25, 0.1, 0.55, 1.0]
SS = 2                          # supersampling of the drawn art
rng = np.random.default_rng(20261112)


def zoom(depth):
    m = RANGE * abs(depth) * SCALE
    return (SH + 2 * m) / N


def to_layer(depth, x, y, d=0.0):
    s = zoom(depth)
    return N / 2 + (x - SW / 2) / s, N / 2 + (y - SH / 2) / s, d / s


# ---------------------------------------------------------------- cut-outs from the NASA images

def disk_bounds(im, thresh=18):
    """bbox of what is brighter than thresh (0..255) on the black sky"""
    a = np.asarray(im.convert("L"))
    ys, xs = np.nonzero(a > thresh)
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def ellipse_alpha(w, h, cx, cy, rx, ry, feather=1.2):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    # distance to the ellipse edge in pixels (approximate: normalised radius times the local radius)
    r = np.sqrt(((xx + 0.5 - cx) / rx) ** 2 + ((yy + 0.5 - cy) / ry) ** 2)
    edge = (1 - r) * min(rx, ry)
    return np.clip(edge / feather + 0.5, 0, 1)


def shade(rgb, alpha, cx, cy, r, light, dark=0.12, soft=0.55):
    """darkens the side of a sphere away from the light (unit vector in the image plane, towards the light)"""
    h, w = alpha.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    nx, ny = (xx + 0.5 - cx) / r, (yy + 0.5 - cy) / r
    nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
    lx, ly, lz = light
    ll = math.sqrt(lx * lx + ly * ly + lz * lz)
    lam = (nx * lx + ny * ly + nz * lz) / ll
    f = dark + (1 - dark) * np.clip((lam + soft) / (1 + soft), 0, 1) ** 1.3
    return np.clip(rgb * f[..., None], 0, 255)


def planet(name, out_d, flatten=1.0, light=None, polar_from_height=False, grow=0.0, rim=None):
    """an RGBA cut-out of the planet in DOWNLOADS/name, out_d px across (equatorial)"""
    im = Image.open(os.path.join(SRC, name)).convert("RGB")
    x0, y0, x1, y1 = disk_bounds(im)
    if polar_from_height:          # a crescent or gibbous disk: the height is the full (polar) diameter
        ry = (y1 - y0) / 2
        rx = ry * flatten
        cx, cy = x0 + rx, (y0 + y1) / 2
    else:
        rx, ry = (x1 - x0) / 2, (y1 - y0) / 2
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    pad = int(max(rx, ry) * (0.08 + grow)) + 4
    box = (int(cx - rx) - pad, int(cy - ry) - pad, int(cx + rx) + pad, int(cy + ry) + pad)
    crop = im.crop(box)
    k = out_d * SS / (2 * rx)
    size = (max(1, round(crop.width * k)), max(1, round(crop.height * k)))
    crop = crop.resize(size, Image.LANCZOS)
    ccx, ccy = (cx - box[0]) * k, (cy - box[1]) * k
    rrx, rry = rx * k * 0.992, ry * k * 0.992     # a hair inside the limb: no dark fringe
    alpha = ellipse_alpha(size[0], size[1], ccx, ccy, rrx, rry, feather=1.2 * SS)
    rgb = np.asarray(crop).astype(np.float32)
    if light is not None:
        rgb = shade(rgb, alpha, ccx, ccy, (rrx + rry) / 2, light)
    rgba = np.dstack([rgb, alpha * 255]).astype(np.uint8)
    img = Image.fromarray(rgba, "RGBA")
    if rim:                         # an atmosphere: a thin glow just outside the limb
        glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
        dr = ImageDraw.Draw(glow)
        w = max(2, int(rrx * 0.035))
        dr.ellipse((ccx - rrx - w, ccy - rry - w, ccx + rrx + w, ccy + rry + w), outline=rim + (150,), width=2 * w)
        glow = glow.filter(ImageFilter.GaussianBlur(w * 1.2))
        glow.alpha_composite(img)
        img = glow
    return img.resize((max(1, img.width // SS), max(1, img.height // SS)), Image.LANCZOS)


def saturn(out_w):
    """Saturn and its rings from PIA06193, mirrored so the light comes from the left like the sun's. The rings take
    their own brightness as alpha (the Cassini division and the planet's shadow on them show the sky); the globe is
    opaque, an ellipse centred on the rings' centre, 0.4406 of the A ring's outer radius across (60,268 km against
    136,775 km), its height down to the lowest lit pixel."""
    im = Image.open(os.path.join(SRC, "PIA06193.jpg")).convert("RGB").transpose(Image.FLIP_LEFT_RIGHT)
    lum0 = np.asarray(im.convert("L")).astype(np.float32)
    x0, y0, x1, y1 = disk_bounds(im, thresh=10)
    ring_r = (x1 - x0) / 2
    cx = (x0 + x1) / 2
    # the rings' centre line: the middle of the lit rows a little in from the outer tip on the lit end
    tip = x0 + int(ring_r * 0.015)
    rows = np.nonzero(lum0[:, tip] > 12)[0]
    if not len(rows):
        tip = x1 - int(ring_r * 0.015)
        rows = np.nonzero(lum0[:, tip] > 12)[0]
    cy = rows.mean()
    rx = ring_r * 0.4406
    ry = y1 - cy
    pad = 40
    box = (int(x0) - pad, int(y0) - pad, int(x1) + pad, int(y1) + pad)
    crop = im.crop(box)
    k = out_w * SS / (x1 - x0)
    crop = crop.resize((round(crop.width * k), round(crop.height * k)), Image.LANCZOS)
    rgb = np.asarray(crop).astype(np.float32)
    lum = rgb.max(axis=2)
    a_rings = np.clip((lum - 6) / 70, 0, 1) ** 0.8
    h, w = lum.shape
    a_body = ellipse_alpha(w, h, (cx - box[0]) * k, (cy - box[1]) * k, rx * k * 0.995, ry * k * 0.995, feather=1.5 * SS)
    alpha = np.maximum(a_rings, a_body)
    safe = np.maximum(alpha, 1e-3)[..., None]
    rgb = np.clip(rgb / np.where(alpha[..., None] > 0.02, safe, 1), 0, 255)
    img = Image.fromarray(np.dstack([rgb, alpha * 255]).astype(np.uint8), "RGBA")
    return img.resize((img.width // SS, img.height // SS), Image.LANCZOS)


def sun(disk_d, glow_d):
    """the SDO AIA 304 Sun, small, inside an orange glow drawn as alpha, with a faint horizontal streak"""
    im = Image.open(os.path.join(SRC, "GSFC_20171208_Archive_e002069.jpg")).convert("RGB")
    x0, y0, x1, y1 = disk_bounds(im, thresh=40)
    r = (y1 - y0) / 2
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    disk = im.crop((int(cx - r), int(cy - r), int(cx + r), int(cy + r))).resize((disk_d, disk_d), Image.LANCZOS)
    a = ellipse_alpha(disk_d, disk_d, disk_d / 2, disk_d / 2, disk_d / 2 * 0.97, disk_d / 2 * 0.97, 1.5)
    # SDO's 304 Angstrom picture is red: its brightness mapped onto orange -> yellow -> white keeps the texture
    lum = np.asarray(disk.convert("L")).astype(np.float32) / 255
    yy, xx = np.mgrid[0:disk_d, 0:disk_d].astype(np.float32)
    rr = np.sqrt((xx - disk_d / 2) ** 2 + (yy - disk_d / 2) ** 2) / (disk_d / 2)
    t = np.clip(lum * 1.6 + np.clip(1 - rr, 0, 1) ** 1.2 * 0.55, 0, 1)[..., None]
    lo, mid, hi = np.array([235, 110, 25]), np.array([255, 200, 80]), np.array([255, 250, 235])
    rgb = np.where(t < 0.5, lo + (mid - lo) * (t / 0.5), mid + (hi - mid) * ((t - 0.5) / 0.5))
    disk = Image.fromarray(np.dstack([rgb, a * 255]).astype(np.uint8), "RGBA")
    G = glow_d
    yy, xx = np.mgrid[0:G, 0:G].astype(np.float32)
    rr = np.sqrt((xx - G / 2) ** 2 + (yy - G / 2) ** 2) / (G / 2)
    core = np.exp(-(rr * 7.0) ** 2)            # the white-hot halo right around the disk
    halo = np.exp(-(rr * 2.4) ** 2) * 0.55     # the wide orange glow
    far = np.clip(1 - rr, 0, 1) ** 3 * 0.25
    streak = np.exp(-((yy - G / 2) / (G * 0.008)) ** 2) * np.exp(-((xx - G / 2) / (G * 0.42)) ** 2) * 0.8
    alpha = np.clip(core + halo + far + streak, 0, 1)
    col = np.zeros((G, G, 3), np.float32)
    t = np.clip(core * 1.4 + streak, 0, 1)[..., None]
    col[:] = np.array([255, 140, 40]) * (1 - t) + np.array([255, 236, 200]) * t
    glow = Image.fromarray(np.dstack([col, alpha * 255]).astype(np.uint8), "RGBA")
    glow.alpha_composite(disk, (G // 2 - disk_d // 2, G // 2 - disk_d // 2))
    return glow


# ---------------------------------------------------------------- the procedural art

def blur_noise(n, cells, sigma):
    small = rng.random((cells, cells)).astype(np.float32)
    im = Image.fromarray((small * 255).astype(np.uint8)).resize((n, n), Image.BICUBIC)
    return np.asarray(im.filter(ImageFilter.GaussianBlur(sigma))).astype(np.float32) / 255


def background():
    """layer 0: deep space, two nebular glows (warm low left, cold high right), a dusty band and the stars"""
    n = N
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32) / n
    img = np.zeros((n, n, 3), np.float32)
    img[:] = np.array([3, 5, 10])
    img += (np.array([10, 22, 44]) * np.clip(1 - np.hypot(xx - 0.80, yy - 0.12) / 0.75, 0, 1)[..., None] ** 1.6)
    # glow positions in the screen layout, mapped into this layer
    def glow(x, y, r, color, power):
        X, Y, R = to_layer(DEPTHS[0], x, y, r)
        d = np.hypot(xx * n - X, yy * n - Y) / R
        return np.array(color, np.float32) * (np.clip(1 - d, 0, 1) ** power)[..., None]
    img += glow(980, 1850, 1200, [110, 50, 14], 2.2)       # warm, behind Jupiter
    img += glow(60, 1450, 900, [60, 26, 8], 2.0)           # warm, low left
    img += glow(980, 300, 1100, [26, 50, 92], 2.0)         # cold, high right
    img += glow(330, 700, 520, [70, 34, 10], 2.0)          # around the sun
    # a faint dusty band across, broken up by noise
    band_d = (yy - 0.30 - (xx - 0.5) * 0.55)
    band = np.exp(-(band_d / 0.11) ** 2)
    neb = blur_noise(n, 24, 18) * 0.6 + blur_noise(n, 80, 6) * 0.4
    img += np.array([12, 16, 30], np.float32) * (band * np.clip(neb * 1.8 - 0.5, 0, 1))[..., None]
    # stars: many faint, few bright; a little colour
    def stars(count, bmin, bmax, sigma, band_bias):
        layer = np.zeros((n, n, 3), np.float32)
        xs = rng.random(count) * n
        ys = rng.random(count) * n
        if band_bias:      # more stars in the band
            keep = rng.random(count) < (0.35 + 0.65 * np.exp(-(((ys / n) - 0.30 - ((xs / n) - 0.5) * 0.55) / 0.13) ** 2))
            xs, ys = xs[keep], ys[keep]
        b = bmin + (bmax - bmin) * rng.random(len(xs)) ** 3
        palette = np.array([[255, 255, 255], [200, 220, 255], [255, 240, 210], [255, 210, 170], [170, 200, 255]],
                           np.float32)
        cols = palette[rng.integers(0, len(palette), len(xs))]
        ix, iy = xs.astype(int), ys.astype(int)
        np.add.at(layer, (iy, ix), cols * b[:, None])
        im = Image.fromarray(np.clip(layer, 0, 255).astype(np.uint8))
        if sigma:
            im = im.filter(ImageFilter.GaussianBlur(sigma))
            return np.asarray(im).astype(np.float32) * (1 + sigma * 3)
        return np.asarray(im).astype(np.float32)
    img += stars(26000, 0.10, 0.55, 0, True)
    img += stars(2600, 0.5, 1.0, 0.7, True)
    img += stars(180, 0.8, 1.0, 1.4, False)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def orbits_layer(sun_xy, orbits, belt):
    """the orbits round the sun (screen layout), drawn on the sun's layer; returns the RGBA layer"""
    d = DEPTHS[1]
    s = zoom(d)
    big = Image.new("RGBA", (N * SS, N * SS), (0, 0, 0, 0))
    dr = ImageDraw.Draw(big)
    sx, sy, _ = to_layer(d, *sun_xy)
    for (a, ratio, ang, color, alpha, width, dashed) in orbits:
        A = a / s * SS
        B = A * ratio
        th = math.radians(ang)
        pts = []
        steps = 1440
        for i in range(steps + 1):
            t = 2 * math.pi * i / steps
            u, v = A * math.cos(t), B * math.sin(t)
            pts.append((sx * SS + u * math.cos(th) - v * math.sin(th), sy * SS + u * math.sin(th) + v * math.cos(th)))
        col = color + (alpha,)
        if dashed:
            for i in range(0, steps, 6):
                dr.line(pts[i:i + 3], fill=col, width=width * SS)
        else:
            dr.line(pts, fill=col, width=width * SS)
    # the asteroid belt: dots scattered round an orbit between Mars's and Jupiter's
    a, ratio, ang, count = belt
    A, th = a / s * SS, math.radians(ang)
    for _ in range(count):
        t = rng.random() * 2 * math.pi
        k = 1 + rng.normal(0, 0.035)
        u, v = A * k * math.cos(t), A * k * ratio * math.sin(t)
        x = sx * SS + u * math.cos(th) - v * math.sin(th)
        y = sy * SS + u * math.sin(th) + v * math.cos(th)
        r = SS * (0.7 + 1.6 * rng.random() ** 3)
        g = int(120 + 100 * rng.random())
        dr.ellipse((x - r, y - r, x + r, y + r), fill=(g + 30, g, g - 30, int(90 + 120 * rng.random())))
    return big.resize((N, N), Image.LANCZOS)


def orbit_radius(sun_xy, p, ratio, ang):
    """the semi-major axis of the orbit (centre sun_xy, ratio, angle) through point p"""
    dx, dy = p[0] - sun_xy[0], p[1] - sun_xy[1]
    th = math.radians(ang)
    u = dx * math.cos(th) + dy * math.sin(th)
    v = -dx * math.sin(th) + dy * math.cos(th)
    return math.hypot(u, v / ratio)


def place(layer, depth, img, x, y):
    """img centred on screen point (x, y), scaled for the layer"""
    X, Y, _ = to_layer(depth, x, y)
    s = zoom(depth)
    w, h = max(1, round(img.width / s)), max(1, round(img.height / s))
    im = img.resize((w, h), Image.LANCZOS)
    layer.alpha_composite(im, (round(X - w / 2), round(Y - h / 2)))


def flatten(layers, w, h, scale=SCALE):
    """the picture at rest on a w x h screen, as Neo draws it"""
    out = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    for im, depth in layers:
        m = math.ceil(RANGE * abs(depth) * scale)
        bw, bh = w + 2 * m, h + 2 * m
        side = max(bw, bh)
        big = im.convert("RGBA").resize((side, side), Image.LANCZOS)
        big = big.crop(((side - bw) // 2, (side - bh) // 2, (side - bw) // 2 + bw, (side - bh) // 2 + bh))
        out.alpha_composite(big, (-m, -m))
    return out.convert("RGB")


def main():
    os.makedirs(OUT, exist_ok=True)
    SUN = (250, 690)
    RATIO, ANG = 0.36, 16

    # the planets, cut out (sizes are screen px)
    light_from_sun = lambda p: (SUN[0] - p[0], SUN[1] - p[1], 600)   # noqa: E731
    EARTH, MOON, MARS, SAT, JUP = (850, 700), (962, 612), (930, 1150), (420, 1250), (1000, 1790)
    earth = planet("as17-148-22727.jpg", 176, light=light_from_sun(EARTH), rim=(110, 170, 255))
    moon = planet("GSFC_20171208_Archive_e000868.jpg", 58, light=light_from_sun(MOON))
    mars = planet("PIA00407.jpg", 118, light=light_from_sun(MARS), rim=(255, 150, 100))
    jup = planet("PIA04866-pj.jpg", 660, flatten=1.069, polar_from_height=True)
    sat = saturn(640).rotate(12, resample=Image.BICUBIC, expand=True)
    sunimg = sun(64, 900)

    # layer 0: the sky
    l0 = background()
    # layer 1: the sun and the orbits through the planets
    orbits = []
    for p, color, alpha, width, dashed in [(EARTH, (110, 160, 230), 70, 2, False),
                                           (MARS, (230, 140, 70), 64, 2, False),
                                           (JUP, (230, 160, 90), 56, 3, False),
                                           (SAT, (140, 170, 220), 56, 2, True)]:
        orbits.append((orbit_radius(SUN, p, RATIO, ANG), RATIO, ANG, color, alpha, width, dashed))
    orbits.append((250, RATIO, ANG, (255, 170, 90), 60, 2, False))        # Mercury's and Venus's, no planets drawn
    orbits.append((390, RATIO, ANG, (220, 190, 150), 48, 2, True))
    belt_a = (orbits[1][0] + orbits[2][0]) / 2
    l1 = orbits_layer(SUN, orbits, (belt_a, RATIO, ANG, 2600))
    place(l1, DEPTHS[1], sunimg, *SUN)
    # layer 2: Earth and Moon, Mars
    l2 = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    place(l2, DEPTHS[2], earth, *EARTH)
    place(l2, DEPTHS[2], moon, *MOON)
    place(l2, DEPTHS[2], mars, *MARS)
    # layer 3: Saturn
    l3 = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    place(l3, DEPTHS[3], sat, *SAT)
    # layer 4: Jupiter, close
    l4 = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    place(l4, DEPTHS[4], jup, *JUP)

    l0.save(os.path.join(OUT, "layer0.jpg"), quality=92)
    for i, l in enumerate([l1, l2, l3, l4], 1):
        l.save(os.path.join(OUT, f"layer{i}.png"), optimize=True)
    json.dump({"range": RANGE, "tilt": TILT,
               "layers": [{"file": f, "depth": d} for f, d in
                          zip(["layer0.jpg", "layer1.png", "layer2.png", "layer3.png", "layer4.png"], DEPTHS)]},
              open(os.path.join(OUT, "parallax.json"), "w"), indent=2)
    layers = [(l0, DEPTHS[0]), (l1, DEPTHS[1]), (l2, DEPTHS[2]), (l3, DEPTHS[3]), (l4, DEPTHS[4])]
    wall = flatten(layers, 1080, 2280)
    wall.save(os.path.join(OUT, "wallpaper.jpg"), quality=93)
    prev = flatten(layers, 1080, 2400)
    side = 1080
    prev.crop((0, 600, side, 600 + side)).resize((512, 512), Image.LANCZOS).save(os.path.join(OUT, "preview.jpg"),
                                                                                quality=90)
    if len(sys.argv) > 3:
        prev.save(os.path.join(sys.argv[3], "solar-system-at-rest-1080x2400.png"))


main()
