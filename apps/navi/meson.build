project('vibeos-navi', 'c', 'cpp', version: '0.1', meson_version: '>= 1.1',
        default_options: ['warning_level=2', 'c_std=gnu11', 'cpp_std=c++20', 'buildtype=release',
                          'cpp_rtti=false', 'b_ndebug=if-release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: ['c', 'cpp'])

fs = import('fs')
i18n = import('i18n')
gnome = import('gnome')

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

# what Navi knows without GTK: paths, Valhalla's routes, the guidance along them, search, the position, the sun, and
# what Back does.
# A static library the app and the tests share.
core_deps = [dependency('gio-2.0', version: '>= 2.76'), dependency('json-glib-1.0'), dependency('libsoup-3.0'),
             meson.get_compiler('c').find_library('m'), dependency('icu-i18n'), dependency('icu-uc')]
# times in the user's language: libvibeos's vibeos-locale (GLib and ICU only), which the app has from libvibeos and
# the tests, without GTK, build in
locale_inc = include_directories('../../libvibeos')
core = static_library('navi-core',
  'src/navi-geo.c', 'src/navi-route.c', 'src/navi-guidance.c', 'src/navi-mvt.c', 'src/navi-search.c',
  'src/navi-search-photon.c', 'src/navi-poi.c', 'src/navi-roadinfo.c', 'src/navi-cameras.c', 'src/navi-position.c', 'src/navi-sun.c',
  'src/navi-compass.c', 'src/navi-back.c',
  c_args: get_option('debug_privacy') ? ['-DNAVI_DEBUG'] : [],
  include_directories: locale_inc,
  dependencies: core_deps)
core_dep = declare_dependency(link_with: core, include_directories: include_directories('src'),
                              dependencies: core_deps)

test('navi', executable('test-navi', 'tests/test-navi.c', '../../libvibeos/vibeos-locale.c', dependencies: core_dep),
     env: ['G_TEST_SRCDIR=' + meson.current_source_dir() / 'tests', 'G_TEST_BUILDDIR=' + meson.current_build_dir(),
           'LANG=C.UTF-8', 'LANGUAGE=', 'LC_ALL=', 'LC_TIME=', 'LC_MESSAGES='])

# the compass as Navi reads it, without Navi's window: claims iio-sensor-proxy's compass, prints the heading for a few
# seconds and releases it (tests/navi-compass-probe.c)
executable('navi-compass-probe', 'tests/navi-compass-probe.c', dependencies: core_dep)

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
# Agent's voices: Piper models run by ONNX Runtime, with espeak-ng's phonemes, and Russian's tiny-voca. Their code is Agent's, built from its
# sources (not copied here), until it is a library both apps link; so is the choice of a language's voice
# (dictate_voice_choose: Agent's settings, then its voices), which Navi has none of its own of.
voice_sources = []
voice_deps = []
voice_src = get_option('voice_src')
if voice_src != ''
  onnxruntime = dependency('libonnxruntime', required: false)
  if not onnxruntime.found()
    onnxruntime = meson.get_compiler('c').find_library('onnxruntime')
  endif
  voice_sources = files(voice_src / 'dictate-voice.c', voice_src / 'dictate-phonemes.c')
  # Russian's voice, tiny-voca, reads its own front end's tokens
  foreach ru : ['', '-numbers', '-tables', '-text', '-normalize', '-stress', '-g2p']
    voice_sources += files(voice_src / 'dictate-ru@0@.c'.format(ru))
  endforeach
  voice_deps = [onnxruntime, dependency('espeak-ng'), dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0')]
  app_args += ['-DHAVE_PIPER', '-I' + voice_src]
  # the search's microphone: Agent's and Dictate's speech recognition (Moonshine, Vosk, the voice activity detector),
  # built from the same sources, its models and recognisers the ones vibeos-dictate's language packages install.
  # Its messages stay in Dictate's translation domain. dictate-gst is the engine's gate on GStreamer's start, which
  # Navi makes itself (gst_init before the engine): that counts as started.
  dictation = static_library('navi-dictation',
    files(voice_src / 'dictate-engine.c', voice_src / 'dictate-models.c', voice_src / 'dictate-moonshine.c',
          voice_src / 'dictate-vosk.c', voice_src / 'dictate-vad.c', voice_src / 'dictate-gst.c'),
    c_args: ['-DGETTEXT_PACKAGE="vibeos-dictate"', '-DLIBRARY_DIR="@0@"'.format(get_option('dictate_library_dir')),
             '-I' + voice_src],
    dependencies: [dependency('gio-2.0'), dependency('json-glib-1.0'), dependency('gstreamer-1.0'),
                   dependency('gstreamer-app-1.0'), onnxruntime, meson.get_compiler('c').find_library('m'),
                   meson.get_compiler('c').find_library('dl')])
  voice_deps += declare_dependency(link_with: dictation)
  app_args += '-DHAVE_DICTATION'
  # a voice not on the phone, downloaded as Agent downloads it (Settings' Voice language): Agent's packs, its dialogs
  # and its stylesheet, into its voices, where either app finds it. Built without Agent's conversation.
  voice_download = static_library('navi-voice-download',
    files(voice_src / 'dictate-download.c', voice_src / 'dictate-download-dialog.c', voice_src / 'dictate-pk.c',
          voice_src / 'dictate-package.c'),
    c_args: ['-DDICTATE_WITHOUT_TALK', '-I' + voice_src],
    dependencies: [dependency('gtk4'), dependency('vibeos-1'), dependency('gio-2.0'), dependency('json-glib-1.0'),
                   dependency('libsoup-3.0'), dependency('libarchive')])
  voice_deps += declare_dependency(link_with: voice_download, dependencies: dependency('libarchive'))
  voice_sources += gnome.compile_resources('navi-voice-resources', 'data/navi-voice.gresource.xml',
                                           source_dir: voice_src, c_name: 'navi_voice_resources')
  # a test's: TEXT spoken into a WAV file, for the microphone's test input (DICTATE_TEST_INPUT)
  executable('navi-say', 'tests/navi-say.c', voice_sources, c_args: ['-I' + voice_src],
             dependencies: [dependency('gio-2.0'), dependency('json-glib-1.0'), meson.get_compiler('c').find_library('m')] +
                           voice_deps,
             install: false)
  # Navi's voice for a language is Agent's (dictate_voice_choose): stand-in voices, Agent's settings schema compiled
  # here, on a memory backend
  dictate_schemas = custom_target('dictate-schemas', input: voice_src / '..' / 'data' / 'vibeos.Dictate.gschema.xml',
                                  output: 'gschemas.compiled',
                                  command: [find_program('glib-compile-schemas'), '--strict', '--targetdir=@OUTDIR@',
                                            voice_src / '..' / 'data'])
  test('voice-choice', executable('test-voice-choice', 'tests/test-voice-choice.c', voice_sources,
                                  c_args: ['-I' + voice_src],
                                  dependencies: [dependency('gio-2.0'), dependency('json-glib-1.0'),
                                                 meson.get_compiler('c').find_library('m')] + voice_deps),
       depends: dictate_schemas,
       env: ['G_TEST_SRCDIR=' + meson.current_source_dir() / 'tests', 'G_TEST_BUILDDIR=' + meson.current_build_dir(),
             'GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir()])
endif
executable('vibeos-navi', 'src/navi-main.c', 'src/navi-window.c', 'src/navi-alerts.c', 'src/navi-panels.c', 'src/navi-draw.c',
           'src/navi-settings.c', 'src/navi-voice.c', 'src/navi-plan.c', voice_sources,
           c_args: app_args,
           dependencies: [core_dep, navi_map_dep, dependency('vibeos-1'), dependency('spongy-gtk-1'),
                          dependency('librsvg-2.0'), speechd] +
                         voice_deps,
           install_rpath: '$ORIGIN/../lib/vibeos-navi',
           install: true)
# the launcher's entry, its name and comment translated from po/
i18n.merge_file(input: 'data/vibeos.Navi.desktop.in', output: 'vibeos.Navi.desktop', type: 'desktop', po_dir: 'po',
                install: true, install_dir: get_option('datadir') / 'applications')
subdir('po')
# D-Bus activation (DBusActivatable): the launcher opens Navi through it, and so does a tap on the planned drive's
# reminder when Navi is not running (its "app.planned-drive" action)
configure_file(input: 'data/vibeos.Navi.service.in', output: 'vibeos.Navi.service',
               configuration: configuration_data({'bindir': get_option('prefix') / get_option('bindir')}),
               install_dir: get_option('datadir') / 'dbus-1' / 'services')
install_data('data/vibeos.Navi.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
# the points of interest's icons: Maki's (CC0), drawn on the markers by the app
install_subdir('data/poi', install_dir: datadir)
