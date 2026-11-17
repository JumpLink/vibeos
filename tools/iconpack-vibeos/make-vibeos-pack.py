#!/usr/bin/python3 -I
# make-vibeos-pack.py OUTDIR GLYPHDIR: the "VibeOS" icon pack for the VibeOS launcher, free to redistribute.
#
# Each icon is a Material Symbols Rounded glyph (Google, Apache-2.0; GLYPHDIR holds <name>_fill1_48px.svg, the glyphs
# the gnome-free pack uses), a Font Awesome Free one ("fa/<name>": fontawesome/<name>.svg beside this script, CC BY
# 4.0) or one drawn for this pack ("own/<name>": own/<name>.svg, filling its box as a Font Awesome glyph does), on a
# squircle tile in the app's colour: white on a coloured tile, coloured on a white one.
# The tile is the launcher's own squircle (icons.c superellipse (): |x|^4 + |y|^4 = 1) at TILE of the canvas, over a
# short drop shadow; the glyph's 48-unit box is EM of the tile. Apps the pack does not draw are put on the pack's
# backplate (a white tile of the same shape and size) by the launcher, at pack.json's scale.
import json
import math
import os
import re
import sys

import cairo
import gi
gi.require_version("Rsvg", "2.0")
from gi.repository import Rsvg
from PIL import Image, ImageFilter

OUT, GLYPHS = sys.argv[1], sys.argv[2]
FA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fontawesome")
OWN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "own")
PX = 256          # icon size, as gnome-free's
SS = 4            # supersampling for the tile's edge and its shadow
TILE = 0.891      # the tile's side over the canvas: 228 of 256 px, 14 px clear all round
EM = 0.73         # the glyph's 48-unit box over the tile's side (a 40-unit glyph spans 61 % of the tile)
FA_EM = 40 / 48   # a Font Awesome (or own) glyph fills its box: it spans what a 40-unit Material glyph does of its 48
EXP = 4           # the squircle's exponent: the launcher's
SHADOW = (3 / 192, 0.5 / 192, 0.22)   # the tile's drop shadow: offset down and blur (sigma) over the canvas, opacity
SCALE = 0.70      # an app the pack does not draw sits on the backplate at this share of the canvas
WHITE = "#FFFFFF"

# drawable: (glyph, background, glyph colour[, lens]). The background is a colour, (top, bottom) for a vertical
# gradient, or (top-left, bottom-right, "diagonal"). A lens, (colour, cx, cy, r) in the glyph's own units, is a disc
# drawn under the glyph: it fills a round hole in it (the camera's lens) a colour of its own. The colours are those of the H2O pack's icon for the same app (named in the
# comment), measured; "no H2O" marks the two apps H2O has no icon for.
ICONS = {
    "phone":      ("fa/phone",          "#51D392",                           WHITE),      # phone
    "messaging":  ("fa/comment",        ("#08CBDD", "#0B93E1"),              WHITE),      # messages
    "contacts":   ("person",            "#008BFF",                           WHITE),      # contacts
    "browser":    ("public",            ("#41BFFA", "#1895F7", "diagonal"),  WHITE),      # browser
    "camera":     ("fa/camera",         "#B71C1C",                           WHITE,       # our own: dark red,
                   ("#000000", 256, 288, 97)),                                                # a black lens
    "calendar":   ("calendar_month",    WHITE,                               "#1E88E5"),  # calendar
    "clock":      ("schedule",          WHITE,                               "#454444"),  # clock
    "compass":    ("explore",           "#243048",                           WHITE),      # h2o_compass
    "calculator": ("calculate",         ("#3F48D2", "#333AD4"),              WHITE),      # calculator
    "settings":   ("settings",          "#425D74",                           WHITE),      # settings
    "weather":    ("partly_cloudy_day", "#FCAD08",                           WHITE),      # weather
    "files":      ("folder",            ("#FFA823", "#FFB923"),              WHITE),      # file_manager
    "terminal":   ("own/terminal",      "#243048",                           WHITE),      # termux (Compass's dark grey, not its black)
    "gallery":    ("photo_library",     ("#1FAFEB", "#2945CA"),              WHITE),      # gallery
    "documents":  ("description",       "#4385F3",                           WHITE),      # google_drive (its blue)
    "video":      ("play_circle",       WHITE,                               "#0D47A1"),  # google_play_movies (dark blue, not its red)
    "store":      ("fa/bag-shopping",   ("#4FD1A5", "#0F7A6B", "diagonal"),  WHITE),      # Software's own: its icon's teal
    "android":    ("android",           "#3DDC84",                           WHITE),      # no H2O: Android's green
    "maps":       ("map",               "#009F55",                           WHITE),      # google_maps (its green)
    "music":      ("music_note",        ("#FAFBF5", "#F9F7F8"),              "#0079B1"),  # music
    "notes":      ("edit_note",         ("#F4C100", "#F4A300"),              WHITE),      # notes
    "recorder":   ("mic",               "#2368FB",                           WHITE),      # recorder
    "dictate":    ("speech_to_text",    "#7C4DFF",                           WHITE),      # no H2O: a violet
    "email":      ("mail",              ("#2AB7E1", "#2C92E3", "diagonal"),  WHITE),      # email
    "download":   ("download",          "#099DFF",                           WHITE),      # download
    "installer":  ("fa/box-open",       "#A0673A",                           WHITE),      # no H2O: cardboard brown
}

# desktop id -> drawable (our apps by their own ids, GNOME's by theirs): gnome-free's map
MAP = {
    "vibeos.Dialer.desktop": "phone", "org.gnome.Calls.desktop": "phone",
    "vibeos.Messaging.desktop": "messaging", "sm.puri.Chatty.desktop": "messaging",
    "vibeos.Contacts.desktop": "contacts", "org.gnome.Contacts.desktop": "contacts",
    "vibeos.Browser.desktop": "browser", "org.gnome.Epiphany.desktop": "browser",
    "firefox-esr.desktop": "browser",
    "vibeos.Camera.desktop": "camera", "org.gnome.Snapshot.desktop": "camera",
    "vibeos.Calendar.desktop": "calendar", "org.gnome.Calendar.desktop": "calendar",
    "vibeos.Clock.desktop": "clock", "org.gnome.clocks.desktop": "clock",
    "vibeos.Compass.desktop": "compass",
    "vibeos.Calculator.desktop": "calculator", "org.gnome.Calculator.desktop": "calculator",
    "vibeos.Settings.desktop": "settings", "org.gnome.Settings.desktop": "settings",
    "vibeos.Weather.desktop": "weather", "org.gnome.Weather.desktop": "weather",
    "vibeos.Files.desktop": "files", "org.gnome.Nautilus.desktop": "files",
    "vibeos.Dictate.desktop": "dictate",
    "vibeos.Installer.desktop": "installer",
    "org.gnome.Console.desktop": "terminal", "org.gnome.Ptyxis.desktop": "terminal",
    "org.gnome.Loupe.desktop": "gallery",
    "org.gnome.Papers.desktop": "documents", "org.gnome.Evince.desktop": "documents",
    "org.gnome.Showtime.desktop": "video", "org.gnome.Totem.desktop": "video", "mpv.desktop": "video",
    "vibeos.Software.desktop": "store", "org.gnome.Software.desktop": "store",
    "vibeos.BootAndroid.desktop": "android", "Waydroid.desktop": "android",
    "org.gnome.Maps.desktop": "maps",
    "org.gnome.Music.desktop": "music", "com.github.neithern.g4music.desktop": "music",
    "org.gnome.TextEditor.desktop": "notes",
    "org.gnome.SoundRecorder.desktop": "recorder",
    "org.gnome.Geary.desktop": "email", "org.gnome.Evolution.desktop": "email",
}


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


def to_pil(s):
    s.flush()
    im = Image.frombuffer("RGBA", (s.get_width(), s.get_height()), bytes(s.get_data()), "raw", "BGRa", 0, 1)
    return im.copy()


def squircle(cr, c, cy, r):
    """the launcher's squircle (icons.c superellipse ()) with half-side r, centred on (c, cy)"""
    steps = 512
    for i in range(steps + 1):
        t = i / steps * 2 * math.pi
        ct, st = math.cos(t), math.sin(t)
        x = math.copysign(abs(ct) ** (2 / EXP), ct)
        y = math.copysign(abs(st) ** (2 / EXP), st)
        (cr.move_to if i == 0 else cr.line_to)(c + r * x, cy + r * y)
    cr.close_path()


def fill(bg, lo, hi):
    """the background as a cairo source over the tile's box lo..hi"""
    if isinstance(bg, str):
        return cairo.SolidPattern(*rgb(bg))
    g = cairo.LinearGradient(lo, lo, hi, hi) if len(bg) == 3 else cairo.LinearGradient(0, lo, 0, hi)
    g.add_color_stop_rgb(0, *rgb(bg[0]))
    g.add_color_stop_rgb(1, *rgb(bg[1]))
    return g


def tile(bg):
    """the squircle tile over its drop shadow, PX x PX, as a PIL image"""
    n = PX * SS
    c, r = n / 2, n * TILE / 2
    dy, blur, alpha = SHADOW
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, n, n)
    cr = cairo.Context(s)
    squircle(cr, c, c + dy * n, r)
    cr.set_source_rgba(0, 0, 0, alpha)
    cr.fill()
    im = to_pil(s).filter(ImageFilter.GaussianBlur(blur * n))
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, n, n)
    cr = cairo.Context(s)
    squircle(cr, c, c, r)
    cr.set_source(fill(bg, c - r, c + r))
    cr.fill()
    im.alpha_composite(to_pil(s))
    return im.resize((PX, PX), Image.LANCZOS)


def _rect(x, y, w, h):
    r = Rsvg.Rectangle()
    r.x, r.y, r.width, r.height = x, y, w, h
    return r


def glyph(name, colour, lens=None):
    """the glyph in its colour, its 48-unit box EM of the tile and centred on it (a Font Awesome or own glyph's box
    FA_EM of that), over its lens if it has one"""
    size = PX * TILE * EM
    if name.startswith("fa/") or name.startswith("own/"):
        folder, base = name.split("/", 1)
        with open(os.path.join(FA if folder == "fa" else OWN, base + ".svg")) as f:
            svg = f.read().replace('fill="currentColor"', 'fill="%s"' % colour)
        size *= FA_EM
    else:
        with open(os.path.join(GLYPHS, name + "_fill1_48px.svg")) as f:
            svg = f.read().replace("<svg ", '<svg fill="%s" ' % colour, 1)
    h = Rsvg.Handle.new_from_data(svg.encode())
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, PX, PX)
    cr = cairo.Context(s)
    off = (PX - size) / 2
    if lens:
        # the view box is fitted into the box, centred, as render_document () does
        lc, cx, cy, r = lens
        vx, vy, vw, vh = (float(v) for v in re.search(r'viewBox="([^"]+)"', svg).group(1).replace(",", " ").split())
        unit = size / max(vw, vh)
        x0, y0 = off + (size - vw * unit) / 2 - vx * unit, off + (size - vh * unit) / 2 - vy * unit
        cr.arc(x0 + cx * unit, y0 + cy * unit, r * unit, 0, 2 * math.pi)
        cr.set_source_rgb(*rgb(lc))
        cr.fill()
    h.render_document(cr, _rect(off, off, size, size))
    return to_pil(s)


def icon(glyph_name, bg, fg, lens=None):
    im = tile(bg)
    im.alpha_composite(glyph(glyph_name, fg, lens))   # the glyph's box lies well inside the tile: nothing to clip
    return im


os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)
for name, (g, bg, fg, *lens) in ICONS.items():
    icon(g, bg, fg, *lens).save(os.path.join(OUT, "icons", name + ".png"), optimize=True)
# the backplate for apps the pack does not draw: the tile, white
tile(WHITE).save(os.path.join(OUT, "icons", "iconback.png"), optimize=True)

with open(os.path.join(OUT, "pack.json"), "w") as f:
    json.dump({"id": "vibeos", "title": "VibeOS", "scale": SCALE, "shape": "squircle",
               "iconback": ["iconback"], "iconmask": [], "iconupon": []}, f, indent=1)
with open(os.path.join(OUT, "gnome-map.json"), "w") as f:
    json.dump(MAP, f, indent=1, sort_keys=True)
with open(os.path.join(OUT, "appfilter.json"), "w") as f:
    json.dump({}, f)
print(len(ICONS), "icons in", OUT)
