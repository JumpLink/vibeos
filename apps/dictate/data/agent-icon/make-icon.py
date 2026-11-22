import sys, math
# Agent's icon (data/vibeos.Agent.svg): python3 make-icon.py head.path ../vibeos.Agent.svg
# Wave ribbons in blue, violet and magenta, scattered at different angles one over another until they fill the tile,
# each a glossy sheet (a light crest deepening into its colour) with a soft shadow on the ones under it; the head in
# white over them.
glyph = open(sys.argv[1]).read().strip()
out = sys.argv[2]

def mix(a, b, t):
    a = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return '#%02X%02X%02X' % tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))

BLUE, VIOLET, MAGENTA = '#1F3BFF', '#7B2CFF', '#F21FC2'
def ramp(t):
    t = max(0.0, min(1.0, t))
    return mix(BLUE, VIOLET, t * 2) if t < 0.5 else mix(VIOLET, MAGENTA, (t - 0.5) * 2)

def lerp(a, b, t): return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
def c(p): return 'M%.2f %.2fC%.2f %.2f %.2f %.2f %.2f %.2f' % (p[0] + p[1] + p[2] + p[3])
def band(A, B):
    return ('M%.2f %.2fC%.2f %.2f %.2f %.2f %.2f %.2f' % (A[0] + A[1] + A[2] + A[3]) +
            'L%.2f %.2fC%.2f %.2f %.2f %.2f %.2f %.2fZ' % (B[3] + B[2] + B[1] + B[0]))

# (angle°, centre y, width, wave height, twist, colour from, colour to, opacity): back to front
RIBBONS = [
    (-24, 26, 44, 34, 0.0, 0.00, 0.30, 1.0),
    (16, 104, 48, 30, 0.5, 0.80, 1.00, 1.0),
    (-6, 66, 34, 38, 1.0, 0.05, 0.40, 0.95),
    (32, 18, 30, 26, 0.8, 0.70, 1.00, 0.92),
    (-34, 112, 32, 28, 0.3, 0.00, 0.25, 0.92),
    (8, 42, 22, 36, 1.2, 0.55, 0.95, 0.9),
    (-16, 92, 20, 32, 0.9, 0.85, 0.45, 0.9),
]

defs, art = [], []
for i, (angle, cy, width, wave, twist, c0, c1, opacity) in enumerate(RIBBONS):
    # the ribbon laid flat across a wide strip, then turned about the tile's centre
    A = [(-40, cy - width / 2), (14, cy - width / 2 - wave), (114, cy - width / 2 + wave), (168, cy - width / 2)]
    # its far edge: the same wave, shifted, and crossing over (a fold) by `twist`
    B = [(-40, cy + width / 2), (14 + 30 * twist, cy + width / 2 - wave * (1 - twist) + wave * twist * 0.6),
         (114 - 30 * twist, cy + width / 2 + wave * (1 - 2 * twist)), (168, cy + width / 2 - width * 0.4 * twist)]
    gid = 'r%d' % i
    defs.append('<linearGradient id="%s" x1="-40" y1="0" x2="168" y2="0" gradientUnits="userSpaceOnUse">'
                '<stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>'
                % (gid, ramp(c0), ramp(c1)))
    top, bottom = cy - width / 2 - wave * 0.75, cy + width / 2 + wave * 0.75
    defs.append('<linearGradient id="g%d" x1="0" y1="%.1f" x2="0" y2="%.1f" gradientUnits="userSpaceOnUse">'
                '<stop offset="0" stop-color="#ffffff" stop-opacity="0.42"/>'
                '<stop offset="0.3" stop-color="#ffffff" stop-opacity="0"/>'
                '<stop offset="0.7" stop-color="#0C0320" stop-opacity="0.05"/>'
                '<stop offset="1" stop-color="#0C0320" stop-opacity="0.42"/></linearGradient>' % (i, top, bottom))
    art.append('<g transform="rotate(%d 64 64)" opacity="%.2f">'
               '<path d="%s" fill="#12052B" fill-opacity="0.5" transform="translate(0 3)" filter="url(#soft)"/>'
               '<path d="%s" fill="url(#%s)"/><path d="%s" fill="url(#g%d)"/>'
               '<path d="%s" fill="none" stroke="#ffffff" stroke-opacity="0.75" stroke-width="1.1" '
               'filter="url(#gleam)"/></g>' % (angle, opacity, band(A, B), band(A, B), gid, band(A, B), i, c(A)))

# the head's shadow is a blurred dark copy under it (feDropShadow is not drawn everywhere)
place = 'translate(64 64) scale(0.36) translate(-146.25 -130.31)'
head = ('<path transform="translate(0 1.6) ' + place + '" fill="#14062E" fill-opacity="0.55" filter="url(#lift)" d="' +
        glyph + '"/><path transform="' + place + '" fill="#ffffff" d="' + glyph + '"/>')
svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128"><!-- Agent\'s icon: a head with circuit lines '
       'in white over scattered blue, violet and magenta wave ribbons, on the tile the other apps use, inset like the '
       'GNOME app icon grid --><defs>'
       '<filter id="soft" x="-10%" y="-30%" width="120%" height="160%"><feGaussianBlur stdDeviation="2.2"/></filter>'
       '<filter id="gleam" x="-10%" y="-50%" width="120%" height="200%"><feGaussianBlur stdDeviation="0.7"/></filter>'
       '<filter id="lift" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="5"/></filter>'
       '<clipPath id="tile"><rect width="128" height="128" rx="28"/></clipPath>'
       '<linearGradient id="bg" x1="0" y1="0" x2="128" y2="128" gradientUnits="userSpaceOnUse"><stop offset="0" '
       'stop-color="' + BLUE + '"/><stop offset="0.5" stop-color="' + VIOLET + '"/><stop offset="1" stop-color="' +
       MAGENTA + '"/></linearGradient>'
       '<radialGradient id="shine" gradientUnits="userSpaceOnUse" cx="28.54" cy="28.54" r="72"><stop offset="0" '
       'stop-color="#fff" stop-opacity="0.098"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>'
       + ''.join(defs) + '</defs><g data-vibeos-inset="1" transform="translate(64 64) scale(0.86) translate(-64 -64)">'
       '<g clip-path="url(#tile)"><rect width="128" height="128" fill="url(#bg)"/>' + ''.join(art) + '</g>'
       '<rect width="128" height="128" rx="28" fill="url(#shine)"/>' + head + '</g></svg>\n')
open(out, 'w').write(svg)
