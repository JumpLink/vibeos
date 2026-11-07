#!/usr/bin/env python3
# vd2svg.py - Android VectorDrawable XML -> SVG, for the apps' icons.
#
#   python3 tools/vd2svg.py OUT_DIR DRAWABLE.xml[=NAME] ...
#
# Each drawable becomes OUT_DIR/NAME.svg (NAME defaults to the file's name). A NAME ending in "-symbolic" makes a
# GTK symbolic icon: every shape is drawn in one colour, which GTK replaces with the widget's CSS colour, as an
# Android icon's tint does. Otherwise the drawable's own colours are kept (theme references such as ?colorPrimary
# cannot be resolved here and become black, with a note on stderr).
#
# The SVG is as large as the drawable in dp (1 dp is 1 GTK px on the phone, at scale 3) and its viewBox is the
# viewport. Groups keep their transforms (Android's order: scale and rotate about the pivot, then translate),
# clip-paths become clipPaths. Fill and stroke follow VectorDrawable.cpp; stroked paths in a symbolic icon are kept
# as strokes, which GTK recolours too. Python 3 stdlib only.

import os
import sys
import xml.etree.ElementTree as ET

A = '{http://schemas.android.com/apk/res/android}'
NOTES = []


def attr(el, name, default=None):
    return el.get(A + name, default)


RES = None     # the res/ directory of the drawable being converted, for @dimen/ references


def dimen(ref):
    """@dimen/x from the res/ dir: the phone's qualifiers first (sw360dp; 1 dp = 3 px), then the base values"""
    name = ref.split('/', 1)[1]
    for values in ('values-sw360dp', 'values'):
        path = os.path.join(RES, values, 'dimens.xml')
        if os.path.exists(path):
            for el in ET.parse(path).getroot().iter('dimen'):
                if el.get('name') == name:
                    return el.text.strip()
    raise SystemExit(f'{ref} not found under {RES}')


def number(el, name, default):
    value = attr(el, name)
    if value is None:
        return default
    if value.startswith('@dimen/'):
        value = dimen(value)
    return float(value.replace('dip', '').replace('dp', ''))


def colour(value, name):
    """An Android colour (#RGB, #ARGB, #RRGGBB, #AARRGGBB) as (svg colour, alpha)."""
    if value is None:
        return None, 1.0
    if not value.startswith('#'):
        if value in ('@android:color/white', '@color/white'):
            return '#ffffff', 1.0
        if value in ('@android:color/black', '@color/black'):
            return '#000000', 1.0
        NOTES.append(f'{name}: colour {value} is a reference: black')
        return '#000000', 1.0
    hexa = value[1:]
    if len(hexa) in (3, 4):
        hexa = ''.join(c * 2 for c in hexa)
    alpha = 1.0
    if len(hexa) == 8:
        alpha = int(hexa[:2], 16) / 255
        hexa = hexa[2:]
    return '#' + hexa.lower(), alpha


def transform(group):
    """VectorDrawable's group matrix: translate(-pivot) scale rotate translate(pivot + translate)."""
    px, py = number(group, 'pivotX', 0), number(group, 'pivotY', 0)
    sx, sy = number(group, 'scaleX', 1), number(group, 'scaleY', 1)
    rotation = number(group, 'rotation', 0)
    tx, ty = number(group, 'translateX', 0), number(group, 'translateY', 0)
    parts = []
    if tx or ty or px or py:
        parts.append(f'translate({px + tx:g} {py + ty:g})')
    if rotation:
        parts.append(f'rotate({rotation:g})')
    if sx != 1 or sy != 1:
        parts.append(f'scale({sx:g} {sy:g})')
    if px or py:
        parts.append(f'translate({-px:g} {-py:g})')
    return ' '.join(parts)


def convert(source, name):
    root = ET.parse(source).getroot()
    if root.tag != 'vector':
        raise SystemExit(f'{source}: <{root.tag}> is not a <vector>')
    symbolic = name.endswith('-symbolic')
    width, height = number(root, 'width', 24), number(root, 'height', 24)
    vw = number(root, 'viewportWidth', width)
    vh = number(root, 'viewportHeight', height)
    clips = []

    def shapes(el, indent):
        out = []
        for child in el:
            if child.tag == 'group':
                inner = shapes(child, indent + '  ')
                matrix = transform(child)
                out.append(f'{indent}<g' + (f' transform="{matrix}"' if matrix else '') + '>')
                out.extend(inner)
                out.append(f'{indent}</g>')
            elif child.tag == 'clip-path':
                # clips everything after it in its group (VectorDrawable.cpp: the group's clip path)
                cid = f'clip{len(clips)}'
                clips.append(f'  <clipPath id="{cid}"><path d="{attr(child, "pathData")}"/></clipPath>')
                rest = shapes(_after(el, child), indent + '  ')
                out.append(f'{indent}<g clip-path="url(#{cid})">')
                out.extend(rest)
                out.append(f'{indent}</g>')
                break
            elif child.tag == 'path':
                out.append(indent + path(child))
        return out

    def path(el):
        d = attr(el, 'pathData')
        fill, fill_alpha = colour(attr(el, 'fillColor'), name)
        stroke, stroke_alpha = colour(attr(el, 'strokeColor'), name)
        fill_alpha *= number(el, 'fillAlpha', 1)
        stroke_alpha *= number(el, 'strokeAlpha', 1)
        width = number(el, 'strokeWidth', 0)
        if symbolic:
            fill = '#000000' if fill else None
            stroke = '#000000' if stroke and width else None
        props = [f'd="{d}"', f'fill="{fill or "none"}"']
        if fill and fill_alpha < 1:
            props.append(f'fill-opacity="{fill_alpha:.3g}"')
        if attr(el, 'fillType') == 'evenOdd':
            props.append('fill-rule="evenodd"')
        if stroke and width:
            props += [f'stroke="{stroke}"', f'stroke-width="{width:g}"',
                      f'stroke-linecap="{attr(el, "strokeLineCap", "butt")}"',
                      f'stroke-linejoin="{attr(el, "strokeLineJoin", "miter")}"']
            if stroke_alpha < 1:
                props.append(f'stroke-opacity="{stroke_alpha:.3g}"')
        return '<path ' + ' '.join(props) + '/>'

    body = shapes(root, '  ')
    alpha = number(root, 'alpha', 1)
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:g}" height="{height:g}" '
             f'viewBox="0 0 {vw:g} {vh:g}">']
    if clips:
        lines += ['  <defs>'] + ['  ' + c for c in clips] + ['  </defs>']
    if alpha < 1:
        lines.append(f'  <g opacity="{alpha:g}">')
    lines += body
    if alpha < 1:
        lines.append('  </g>')
    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


def _after(parent, child):
    """the siblings after `child`, as a pseudo-element"""
    rest = ET.Element('group')
    seen = False
    for el in parent:
        if seen:
            rest.append(el)
        seen = seen or el is child
    return rest


def main():
    if len(sys.argv) < 3:
        raise SystemExit('usage: vd2svg.py OUT_DIR DRAWABLE.xml[=NAME] ...')
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    for arg in sys.argv[2:]:
        source, _, name = arg.partition('=')
        name = name or os.path.splitext(os.path.basename(source))[0]
        global RES
        RES = os.path.dirname(os.path.dirname(os.path.abspath(source)))
        with open(os.path.join(out_dir, name + '.svg'), 'w') as f:
            f.write(convert(source, name))
    for note in NOTES:
        print('vd2svg:', note, file=sys.stderr)


if __name__ == '__main__':
    main()
