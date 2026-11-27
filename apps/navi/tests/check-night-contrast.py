#!/usr/bin/env python3
"""check-night-contrast.py STYLE-NIGHT.json SRCDIR: the night map stays readable, now that every drive is on it, by
day too: WCAG contrast ratios (text 4.5:1, a line or a shape 3:1) of what is drawn over the night style, from the
colours as Navi has them (the style's palette, the route's in navi-window.c, the car's in navi-draw.c)."""
import json, re, sys

def lum(h):
    h = h.lstrip('#')
    def f(c):
        c /= 255
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)

def ratio(a, b):
    hi, lo = sorted((lum(a), lum(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)

style = json.load(open(sys.argv[1]))
src = sys.argv[2]
paint = {l['id'].rsplit('--', 1)[0]: l.get('paint', {}) for l in style['layers']}
ground = paint['background']['background-color']
road, motorway = paint['road-minor']['line-color'], paint['road-motorway']['line-color']
water, park = paint['water']['fill-color'], paint['park']['fill-color']
label, halo = paint['road-label']['text-color'], paint['road-label']['text-halo-color']

window = open(f'{src}/src/navi-window.c').read()
def pair(name):
    m = re.search(name + r'\[2\] = \{ "(#\w{6})", "(#\w{6})" \}', window)
    assert m, name
    return m.group(2)  # [1]: the night's
route_fill, route_edge = pair('route_fill'), pair('route_edge')
draw = open(f'{src}/src/navi-draw.c').read()
car = dict(re.findall(r'(CAR_\w+)\[\] = "(#\w{6})"', draw))

checks = [  # (what, colour, on, at least)
    ('street names', label, halo, 4.5),
    ('street names on the ground', label, ground, 4.5),
    ('the route on the ground', route_fill, ground, 3),
    ('the route on a street', route_fill, road, 3),
    ('the route on a motorway', route_fill, motorway, 3),
    ('the route on water', route_fill, water, 3),
    ('the route on a park', route_fill, park, 3),
    ('the turn arrow (white) in its edge', '#FFFFFF', route_edge, 3),
    ('a camera or a signal (white ring) on the ground', '#FFFFFF', ground, 3),
    ('a camera or a signal on a motorway', '#FFFFFF', motorway, 3),
    ('the car on the ground', car['CAR_BODY'], ground, 3),
    ('the car on water', car['CAR_BODY'], water, 3),
    ('the car\'s rim on the route', car['CAR_RIM_NIGHT'], route_fill, 3),
    ('the car\'s rim around it', car['CAR_RIM_NIGHT'], car['CAR_BODY'], 3),
]
failed = 0
for what, colour, on, least in checks:
    r = ratio(colour, on)
    ok = r >= least
    failed += not ok
    print(f'{"ok" if ok else "NOT OK"} {r:5.1f}:1 (>= {least}) {what}: {colour} on {on}')
sys.exit(1 if failed else 0)
