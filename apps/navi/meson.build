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
