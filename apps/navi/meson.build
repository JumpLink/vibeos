project('vibeos-navi', 'c', 'cpp', version: '0.1', meson_version: '>= 1.1',
        default_options: ['warning_level=2', 'c_std=gnu11', 'cpp_std=c++20', 'buildtype=release',
                          'cpp_rtti=false', 'b_ndebug=if-release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: ['c', 'cpp'])

fs = import('fs')

# MapLibre Native, built and staged by maplibre/build-maplibre.sh: static archives, headers, and the
# definitions and link line its core was built with
stage = get_option('maplibre_stage')
if stage == ''
  error('-Dmaplibre_stage=DIR is required: the stage-* directory maplibre/build-maplibre.sh made')
endif
maplibre_args = []
foreach d : fs.read(stage / 'maplibre.defines').strip().split('\n')
  maplibre_args += '-D' + d
endforeach
maplibre_link = []
foreach l : fs.read(stage / 'maplibre.libs').strip().split('\n')
  maplibre_link += l.startswith('-') ? l : stage / l
endforeach
maplibre = declare_dependency(
  # -isystem: MapLibre's headers are not ours to warn about
  compile_args: ['-isystem', stage / 'include'] + maplibre_args,
  link_args: maplibre_link,
  dependencies: [dependency('threads')])

gtk = dependency('gtk4', version: '>= 4.14')
# MapLibre first: its archives bundle harfbuzz and freetype, and they must win over the system's
# libharfbuzz that GTK brings in. With GTK first, the linker bound MapLibre's hb_* calls to the shared
# harfbuzz while FT_* stayed bundled: a system hb_ft_font_create() handed a bundled FT_Face.
deps = [maplibre, gtk, dependency('egl'), dependency('glesv2'), dependency('libuv'),
        meson.get_compiler('c').find_library('m')]

# The widget and all of MapLibre in one shared library in a private directory (nothing goes to the
# system libdir); a version script exports only the navi_map_* C API.
privlibdir = get_option('prefix') / 'lib' / 'vibeos-navi'
symbols = meson.current_source_dir() / 'src' / 'navi-map.sym'
navi_map = shared_library('vibeos-navi-map',
  'src/navi-map.cpp', 'src/navi-backend.cpp', 'src/navi-runloop.cpp',
  dependencies: deps,
  cpp_args: get_option('debug_privacy') ? ['-DNAVI_DEBUG'] : [],
  link_args: ['-Wl,--version-script=' + symbols, '-Wl,--exclude-libs,ALL', '-Wl,--gc-sections'],
  link_depends: symbols,
  soversion: '0',
  install: true, install_dir: privlibdir)
navi_map_dep = declare_dependency(link_with: navi_map, include_directories: include_directories('src'),
                                  dependencies: gtk)
install_headers('src/navi-map.h', subdir: 'vibeos-navi')

executable('navi-map-demo', 'demo/navi-map-demo.c',
  dependencies: [navi_map_dep, dependency('json-glib-1.0'), meson.get_compiler('c').find_library('m')],
  install_rpath: '$ORIGIN/../lib/vibeos-navi',
  install: true)
install_data('demo/berlin-tiergarten.geojson', install_dir: get_option('datadir') / 'vibeos-navi')

# ---- the app ----

# what Navi knows without GTK: paths, Valhalla's routes, the guidance along them, search, the position, the sun.
# A static library the app and the tests share.
core_deps = [dependency('gio-2.0', version: '>= 2.76'), dependency('json-glib-1.0'), dependency('libsoup-3.0'),
             meson.get_compiler('c').find_library('m')]
core = static_library('navi-core',
  'src/navi-geo.c', 'src/navi-route.c', 'src/navi-guidance.c', 'src/navi-mvt.c', 'src/navi-search.c',
  'src/navi-search-photon.c', 'src/navi-poi.c', 'src/navi-roadinfo.c', 'src/navi-position.c', 'src/navi-sun.c',
  c_args: get_option('debug_privacy') ? ['-DNAVI_DEBUG'] : [],
  dependencies: core_deps)
core_dep = declare_dependency(link_with: core, include_directories: include_directories('src'),
                              dependencies: core_deps)

test('navi', executable('test-navi', 'tests/test-navi.c', dependencies: core_dep),
     env: ['G_TEST_SRCDIR=' + meson.current_source_dir() / 'tests', 'G_TEST_BUILDDIR=' + meson.current_build_dir()])

# the map's styles, day and night, made from the generator with the server's tile and glyph URLs
python = find_program('python3')
datadir = get_option('prefix') / get_option('datadir') / 'vibeos-navi'
foreach mode : ['day', 'night']
  custom_target('style-' + mode, output: 'style-' + mode + '.json', input: 'style/navi-style.py',
                command: [python, '@INPUT@', mode, '@OUTPUT@', get_option('server')],
                install: true, install_dir: datadir)
endforeach

# the app: the window and its panels over the map widget and the core
speechd = dependency('speech-dispatcher', required: false)
app_args = ['-DGETTEXT_PACKAGE="vibeos-navi"', '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
            '-DPKGDATADIR="@0@"'.format(datadir)]
if speechd.found()
  app_args += '-DHAVE_SPEECHD'
endif
if get_option('debug_privacy')
  app_args += '-DNAVI_DEBUG'
endif
# Agent's voices: Piper models run by ONNX Runtime, with espeak-ng's phonemes. Their code is Agent's, built from its
# sources (not copied here), until it is a library both apps link.
voice_sources = []
voice_deps = []
voice_src = get_option('voice_src')
if voice_src != ''
  onnxruntime = dependency('libonnxruntime', required: false)
  if not onnxruntime.found()
    onnxruntime = meson.get_compiler('c').find_library('onnxruntime')
  endif
  voice_sources = files(voice_src / 'dictate-voice.c', voice_src / 'dictate-phonemes.c')
  voice_deps = [onnxruntime, dependency('espeak-ng'), dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0')]
  app_args += ['-DHAVE_PIPER', '-I' + voice_src]
endif
executable('vibeos-navi', 'src/navi-main.c', 'src/navi-window.c', 'src/navi-panels.c', 'src/navi-draw.c',
           'src/navi-settings.c', 'src/navi-voice.c', voice_sources,
           c_args: app_args,
           dependencies: [core_dep, navi_map_dep, dependency('vibeos-1'), dependency('spongy-gtk-1'),
                          dependency('librsvg-2.0'), speechd] +
                         voice_deps,
           install_rpath: '$ORIGIN/../lib/vibeos-navi',
           install: true)
install_data('data/vibeos.Navi.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/vibeos.Navi.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
# the points of interest's icons: Maki's (CC0), drawn on the markers by the app
install_subdir('data/poi', install_dir: datadir)
