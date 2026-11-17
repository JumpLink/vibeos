#!/usr/bin/python3 -I
# make-gnome-free-pack.py OUTDIR GLYPHDIR: the "gnome-free" icon pack for the VibeOS launcher (called Round before
# VibeOS), free to redistribute.
#
# Each icon is a white Material Symbols Rounded glyph (Google, Apache-2.0; fetched into GLYPHDIR as
# <name>_fill1_48px.svg) on a round gradient in the app's own colour. Apps the pack does not draw are put on the
# pack's backplate (a light disc) by VibeOS, at pack.json's scale.
import json
import os
import sys

import cairo
import gi
gi.require_version("Rsvg", "2.0")
from gi.repository import Rsvg
from PIL import Image, ImageFilter

OUT, GLYPHS = sys.argv[1], sys.argv[2]
PX = 256          # icon size
SS = 4            # supersampling for the disc's edge

# drawable: (glyph, top-left colour, bottom-right colour)
ICONS = {
    "phone":      ("call",              "#4ADE80", "#16A34A"),
    "messaging":  ("chat",              "#60A5FA", "#1D4ED8"),
    "contacts":   ("person",            "#FDBA74", "#EA580C"),
    "browser":    ("public",            "#A5B4FC", "#4F46E5"),
    "camera":     ("photo_camera",      "#6B7280", "#1F2937"),
    "calendar":   ("calendar_month",    "#FCA5A5", "#DC2626"),
    "clock":      ("schedule",          "#475569", "#0F172A"),
    "compass":    ("explore",           "#5EEAD4", "#0D9488"),
    "calculator": ("calculate",         "#C4B5FD", "#7C3AED"),
    "settings":   ("settings",          "#A8B3C4", "#526077"),
    "weather":    ("partly_cloudy_day", "#7DD3FC", "#0284C7"),
    "files":      ("folder",            "#FDE68A", "#F59E0B"),
    "terminal":   ("terminal",          "#404040", "#0A0A0A"),
    "gallery":    ("photo_library",     "#F9A8D4", "#DB2777"),
    "documents":  ("description",       "#BEF264", "#4D7C0F"),
    "video":      ("play_circle",       "#F0ABFC", "#A21CAF"),
    "store":      ("shopping_bag",      "#67E8F9", "#4F46E5"),
    "android":    ("android",           "#86EFAC", "#15803D"),
    "maps":       ("map",               "#86EFAC", "#0F766E"),
    "music":      ("music_note",        "#FDA4AF", "#E11D48"),
    "notes":      ("edit_note",         "#FDE047", "#CA8A04"),
    "recorder":   ("mic",               "#FCA5A5", "#B91C1C"),
    "dictate":    ("speech_to_text",    "#A5F3FC", "#0E7490"),
    "email":      ("mail",              "#93C5FD", "#2563EB"),
    "download":   ("download",          "#A7F3D0", "#059669"),
    "installer":  ("package_2",         "#E7B98A", "#9A5B2E"),
}

# desktop id -> drawable (our apps by their own ids, GNOME's by theirs)
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
    "org.gnome.Software.desktop": "store",
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


def disc(c1, c2):
    """the gradient disc with a soft top light, PX x PX, as a PIL image"""
    n = PX * SS
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, n, n)
    cr = cairo.Context(s)
    cr.arc(n / 2, n / 2, n / 2, 0, 6.283185307)
    cr.clip()
    g = cairo.LinearGradient(0, 0, n, n)
    g.add_color_stop_rgb(0, *rgb(c1))
    g.add_color_stop_rgb(1, *rgb(c2))
    cr.set_source(g)
    cr.paint()
    light = cairo.RadialGradient(n * 0.35, n * 0.18, 0, n * 0.35, n * 0.18, n * 0.75)
    light.add_color_stop_rgba(0, 1, 1, 1, 0.22)
    light.add_color_stop_rgba(1, 1, 1, 1, 0)
    cr.set_source(light)
    cr.paint()
    return to_pil(s).resize((PX, PX), Image.LANCZOS)


def to_pil(s):
    s.flush()
    im = Image.frombuffer("RGBA", (s.get_width(), s.get_height()), bytes(s.get_data()), "raw", "BGRa", 0, 1)
    return im.copy()


def glyph(name, size, colour):
    with open(os.path.join(GLYPHS, name + "_fill1_48px.svg")) as f:
        svg = f.read().replace("<svg ", '<svg fill="%s" ' % colour, 1)
    h = Rsvg.Handle.new_from_data(svg.encode())
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, PX, PX)
    cr = cairo.Context(s)
    off = (PX - size) / 2
    h.render_document(cr, Rsvg.Rectangle() if False else _rect(off, off, size, size))
    return to_pil(s)


def _rect(x, y, w, h):
    r = Rsvg.Rectangle()
    r.x, r.y, r.width, r.height = x, y, w, h
    return r


def icon(glyph_name, c1, c2):
    im = disc(c1, c2)
    size = int(PX * 0.52)
    shadow = glyph(glyph_name, size, "#000000")
    alpha = shadow.getchannel("A").point(lambda a: a * 0.28)
    shadow.putalpha(alpha)
    shadow = shadow.filter(ImageFilter.GaussianBlur(PX / 64))
    layer = Image.new("RGBA", (PX, PX))
    layer.alpha_composite(shadow, (0, int(PX / 64)))
    layer.alpha_composite(glyph(glyph_name, size, "#FFFFFF"))
    # the glyph and its shadow stay inside the disc
    mask = disc("#000000", "#000000").getchannel("A")
    clipped = Image.new("RGBA", (PX, PX))
    clipped.paste(layer, (0, 0), mask)
    im.alpha_composite(clipped)
    return im


os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)
for name, (g, c1, c2) in ICONS.items():
    icon(g, c1, c2).save(os.path.join(OUT, "icons", name + ".png"), optimize=True)
# the backplate for apps the pack does not draw
disc("#F8FAFC", "#E2E8F0").save(os.path.join(OUT, "icons", "iconback.png"), optimize=True)

with open(os.path.join(OUT, "pack.json"), "w") as f:
    json.dump({"id": "gnome-free", "title": "gnome-free", "scale": 0.62, "shape": None,
               "iconback": ["iconback"], "iconmask": [], "iconupon": []}, f, indent=1)
with open(os.path.join(OUT, "gnome-map.json"), "w") as f:
    json.dump(MAP, f, indent=1, sort_keys=True)
with open(os.path.join(OUT, "appfilter.json"), "w") as f:
    json.dump({}, f)
print(len(ICONS), "icons in", OUT)
