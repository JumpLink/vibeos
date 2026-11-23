#!/usr/bin/env python3
"""navi-style.py day|night OUT.json [SERVER]: Navi's map style, Waze's look over OpenMapTiles 3.16 vector tiles.

Day: a near-white ground, grey raised roads, mint parks, cyan water, grey labels. Night: a navy ground, blue-grey
streets, dark green parks. Street names stand upright on the screen, as Waze's do. One set of layers per country
source (the map server keeps one tile archive per country); the map takes a source's layers out of the style while
the view is far from its bounds. SERVER is the tile and glyph server, https://navi.vibeos.top by default."""
import json, sys

COUNTRIES = [  # name, bounds (west, south, east, north)
    ('germany', [5.8, 47.2, 15.1, 55.1]), ('poland', [14.0, 48.9, 24.2, 55.0]),
    ('czech-republic', [12.0, 48.5, 18.9, 51.1]), ('netherlands', [3.3, 50.7, 7.3, 53.7]),
    ('switzerland', [5.9, 45.8, 10.5, 47.9]), ('austria', [9.4, 46.3, 17.2, 49.1]),
    ('slovakia', [16.8, 47.7, 22.6, 49.7]), ('great-britain', [-8.7, 49.8, 1.8, 61.1]),
    ('ukraine', [22.1, 44.3, 40.3, 52.4]), ('israel-and-palestine', [34.2, 29.4, 35.9, 33.4]),
    ('spain', [-18.3, 27.5, 4.4, 43.9]), ('france', [-7.0, 41.2, 10.1, 51.5]), ('italy', [6.6, 35.0, 19.2, 47.2]),
    ('us', [-180.0, 15.9, -64.5, 73.0]), ('japan', [122.5, 20.0, 154.5, 45.9]), ('belgium', [2.3, 49.4, 6.5, 51.6]),
    ('portugal', [-33.7, 29.2, -6.1, 42.2]), ('sweden', [10.5, 55.0, 24.3, 69.1]),
    ('denmark', [7.7, 54.4, 15.7, 58.1]), ('norway', [-11.4, 57.5, 35.6, 81.1]), ('serbia', [18.8, 42.2, 23.1, 46.2]),
    ('slovenia', [13.3, 45.4, 16.7, 46.9]), ('belarus', [23.1, 51.2, 32.8, 56.2]),
    ('bulgaria', [22.3, 41.2, 29.2, 44.3]), ('estonia', [20.8, 57.4, 28.3, 60.0]), ('latvia', [19.7, 55.6, 28.3, 58.1]),
    ('lithuania', [20.6, 53.8, 26.9, 56.5]), ('andorra', [1.4, 42.4, 1.8, 42.7]), ('moldova', [26.6, 45.4, 30.2, 48.5]),
    ('finland', [19.0, 59.2, 31.7, 70.1]), ('albania', [18.8, 39.6, 21.1, 42.7]),
    ('bosnia-herzegovina', [15.7, 42.5, 19.7, 45.3]), ('croatia', [13.0, 42.1, 19.5, 46.6]),
    ('cyprus', [31.9, 34.2, 35.0, 36.1]), ('greece', [18.9, 34.5, 29.7, 41.8]), ('hungary', [16.1, 45.7, 23.0, 48.6]),
    ('iceland', [-25.8, 62.8, -12.4, 67.6]), ('ireland-and-northern-ireland', [-16.4, 47.9, -5.0, 56.9]),
    ('kosovo', [20.0, 41.8, 21.8, 43.3]), ('liechtenstein', [9.4, 47.0, 9.7, 47.3]),
    ('luxembourg', [5.7, 49.4, 6.6, 50.2]), ('macedonia', [20.4, 40.8, 23.1, 42.4]), ('malta', [13.8, 35.5, 14.9, 36.4]),
    ('monaco', [7.4, 43.4, 7.6, 43.8]), ('montenegro', [18.1, 41.6, 20.4, 43.6]), ('romania', [20.2, 43.6, 30.3, 48.3]),
    ('turkey', [25.5, 35.7, 44.9, 43.1]), ('australia', [68.1, -57.1, 169.1, -8.8]),
    ('malaysia-singapore-brunei', [99.2, 0.8, 119.7, 7.8]), ('new-zealand', [-180.0, -56.8, 180.0, -28.4]),
]

PALETTES = {
    'day': dict(
        ground='#FAFCF9', park='#BAF0C6', wood='#AEEABC', water='#9CE4FC', building='#EFF1EE',
        road='#C2C6CA', road_edge='#ADB2B7', motorway='#B6BCC2', motorway_edge='#A2A9B0',
        rail='#C9CDD0', rail_tie='#EEF0EE', label='#5E6368', label_halo='#FFFFFF', place='#7E8489',
        city='#55595D', water_label='#4BA8D0', border='#C0B8D0'),
    # sampled from Waze's night map: the ground #272E3A, blocks #2D3E53, streets #3C546C
    'night': dict(
        ground='#272E3A', park='#27493D', wood='#25443A', water='#1B4B68', building='#2D3E53',
        road='#3C546C', road_edge='#2A3A4D', motorway='#4A6680', motorway_edge='#2F4359',
        rail='#3A4656', rail_tie='#2B3442', label='#C3CCD6', label_halo='#1E242E', place='#9AA6B3',
        city='#D5DCE3', water_label='#6FB7DA', border='#5F5A78'),
}

mode = sys.argv[1]
out = sys.argv[2]
server = (sys.argv[3] if len(sys.argv) > 3 else 'https://navi.vibeos.top').rstrip('/')
C = PALETTES[mode]

def z(*stops):            # an exponential width by zoom, base 1.6: [zoom, width, ...]
    return ['interpolate', ['exponential', 1.6], ['zoom'], *stops]

def wider(width, by):     # the same widths, `by` px more at every stop (a zoom curve cannot be added to)
    return width[:3] + [v + by if i % 2 else v for i, v in enumerate(width[3:])]

ROADS = [   # (name, filter, min zoom, widths of the fill by zoom, fill, edge)
    ('minor', ['in', 'class', 'minor', 'service'], 13, z(13, 1, 15, 5, 17, 17, 19, 60), C['road'], C['road_edge']),
    ('secondary', ['in', 'class', 'secondary', 'tertiary'], 10, z(10, 0.6, 13, 2.4, 15, 7.5, 17, 23, 19, 76), C['road'], C['road_edge']),
    ('primary', ['in', 'class', 'primary', 'trunk'], 7, z(7, 0.5, 10, 1.4, 13, 3.4, 15, 9.5, 17, 28, 19, 90), C['road'], C['road_edge']),
    ('motorway', ['==', 'class', 'motorway'], 5, z(5, 0.5, 9, 1.6, 13, 4, 15, 11, 17, 32, 19, 100), C['motorway'], C['motorway_edge']),
]

layers = [{'id': 'background', 'type': 'background', 'paint': {'background-color': C['ground']}}]
sources = {}
for n, bounds in COUNTRIES:
    sources[n] = {'type': 'vector', 'tiles': [f'{server}/tiles/{n}/{{z}}/{{x}}/{{y}}.mvt'], 'minzoom': 0,
                  'maxzoom': 14, 'bounds': bounds,
                  'attribution': '© OpenMapTiles © OpenStreetMap contributors'}
    s = lambda layer, **kw: dict(source=n, **{'source-layer': layer}, **kw)
    layers += [
        {'id': f'wood--{n}', 'type': 'fill', **s('landcover'), 'filter': ['in', 'class', 'wood', 'forest'],
         'paint': {'fill-color': C['wood']}},
        {'id': f'grass--{n}', 'type': 'fill', **s('landcover'), 'filter': ['in', 'class', 'grass', 'park', 'meadow'],
         'paint': {'fill-color': C['park']}},
        {'id': f'park--{n}', 'type': 'fill', **s('park'), 'paint': {'fill-color': C['park']}},
        {'id': f'water--{n}', 'type': 'fill', **s('water'), 'filter': ['!=', 'brunnel', 'tunnel'],
         'paint': {'fill-color': C['water']}},
        {'id': f'waterway--{n}', 'type': 'line', **s('waterway'), 'filter': ['!=', 'brunnel', 'tunnel'],
         'paint': {'line-color': C['water'], 'line-width': z(10, 0.6, 15, 3, 18, 10)}},
        {'id': f'building--{n}', 'type': 'fill', **s('building'), 'minzoom': 15.5,
         'paint': {'fill-color': C['building'], 'fill-opacity': ['interpolate', ['linear'], ['zoom'], 15.5, 0, 16.5, 0.85]}},
    ]
    # tunnels under everything else, faint
    for name, flt, mz, width, fill, edge in ROADS:
        layers.append({'id': f'tunnel-{name}--{n}', 'type': 'line', **s('transportation'), 'minzoom': mz,
                       'filter': ['all', flt, ['==', 'brunnel', 'tunnel']],
                       'layout': {'line-cap': 'butt', 'line-join': 'round'},
                       'paint': {'line-color': fill, 'line-width': width, 'line-opacity': 0.45}})
    layers += [
        {'id': f'rail--{n}', 'type': 'line', **s('transportation'), 'minzoom': 12,
         'filter': ['all', ['==', 'class', 'rail'], ['!=', 'brunnel', 'tunnel']],
         'paint': {'line-color': C['rail'], 'line-width': z(12, 1, 16, 4, 19, 10)}},
        {'id': f'rail-ties--{n}', 'type': 'line', **s('transportation'), 'minzoom': 14,
         'filter': ['all', ['==', 'class', 'rail'], ['!=', 'brunnel', 'tunnel']],
         'paint': {'line-color': C['rail_tie'], 'line-width': z(14, 1.5, 16, 3, 19, 8),
                   'line-dasharray': [0.25, 1.25]}},
    ]
    # the roads: an edge a little wider than the fill, drawn first for all classes, so crossings join cleanly
    not_tunnel = ['!=', 'brunnel', 'tunnel']
    for name, flt, mz, width, fill, edge in ROADS:
        layers.append({'id': f'road-{name}-edge--{n}', 'type': 'line', **s('transportation'), 'minzoom': mz,
                       'filter': ['all', flt, not_tunnel], 'layout': {'line-cap': 'round', 'line-join': 'round'},
                       'paint': {'line-color': edge, 'line-width': wider(width, 1.5)}})
    for name, flt, mz, width, fill, edge in ROADS:
        layers.append({'id': f'road-{name}--{n}', 'type': 'line', **s('transportation'), 'minzoom': mz,
                       'filter': ['all', flt, not_tunnel], 'layout': {'line-cap': 'round', 'line-join': 'round'},
                       'paint': {'line-color': fill, 'line-width': width,
                                 # the raised look: the fill sits a hair up the screen from its edge
                                 'line-translate': [0, -1], 'line-translate-anchor': 'viewport'}})
    layers += [
        {'id': f'border--{n}', 'type': 'line', **s('boundary'), 'filter': ['<=', 'admin_level', 2],
         'paint': {'line-color': C['border'], 'line-width': 1.2, 'line-dasharray': [3, 2]}},
        {'id': f'road-label--{n}', 'type': 'symbol', **s('transportation_name'), 'minzoom': 13,
         'filter': ['in', 'class', 'motorway', 'trunk', 'primary', 'secondary', 'tertiary', 'minor'],
         # Waze's street names stand upright on the screen, not bent along the street, every few hundred pixels of it
         'layout': {'symbol-placement': 'line', 'symbol-spacing': 380,
                    'text-field': ['coalesce', ['get', 'name:latin'], ['get', 'name']],
                    'text-font': ['Noto Sans Bold'], 'text-size': ['interpolate', ['linear'], ['zoom'], 13, 12, 16, 15, 18, 19, 19, 21],
                    'text-pitch-alignment': 'viewport', 'text-rotation-alignment': 'viewport', 'text-padding': 12},
         'paint': {'text-color': C['label'], 'text-halo-color': C['label_halo'], 'text-halo-width': 2}},
        {'id': f'water-label--{n}', 'type': 'symbol', **s('water_name'), 'minzoom': 12,
         'layout': {'text-field': ['get', 'name'], 'text-font': ['Noto Sans Regular'], 'text-size': 12},
         'paint': {'text-color': C['water_label'], 'text-halo-color': C['label_halo'], 'text-halo-width': 1.5}},
        {'id': f'place-suburb--{n}', 'type': 'symbol', **s('place'), 'minzoom': 12, 'maxzoom': 16,
         'filter': ['in', 'class', 'suburb', 'neighbourhood', 'quarter'],
         'layout': {'text-field': ['get', 'name'], 'text-font': ['Noto Sans Bold'], 'text-size': 15,
                    'text-pitch-alignment': 'viewport'},
         'paint': {'text-color': C['place'], 'text-halo-color': C['label_halo'], 'text-halo-width': 1.5}},
        {'id': f'place-town--{n}', 'type': 'symbol', **s('place'), 'minzoom': 8, 'maxzoom': 15,
         'filter': ['in', 'class', 'town', 'village'],
         'layout': {'text-field': ['get', 'name'], 'text-font': ['Noto Sans Bold'], 'text-size': 13},
         'paint': {'text-color': C['place'], 'text-halo-color': C['label_halo'], 'text-halo-width': 1.5}},
        {'id': f'place-city--{n}', 'type': 'symbol', **s('place'), 'minzoom': 4, 'maxzoom': 14,
         'filter': ['==', 'class', 'city'],
         'layout': {'text-field': ['get', 'name'], 'text-font': ['Noto Sans Bold'], 'text-size': 16},
         'paint': {'text-color': C['city'], 'text-halo-color': C['label_halo'], 'text-halo-width': 2}},
    ]

style = {'version': 8, 'name': f'navi-{mode}', 'glyphs': f'{server}/glyphs/{{fontstack}}/{{range}}.pbf',
         'sources': sources, 'layers': layers}
with open(out, 'w') as f:
    json.dump(style, f, separators=(',', ':'))
