# libtonal: Material 3's colour schemes from a seed colour or a wallpaper, computed the way Android computes them
pkg = import('pkgconfig')
glib = dependency('glib-2.0', version: '>= 2.74')
# Every multiply and add rounded on its own, as in Material's own code: on arm64 GCC otherwise fuses them, and a few
# colours land one step away from Android's (fidelity and content schemes, the test's reference digests)
tonal_args = meson.get_compiler('c').get_supported_arguments('-ffp-contract=off')
tonal_lib = shared_library('tonal-1', 'hct.c', 'palette.c', 'scheme.c', 'roles.c', 'quantize.c', 'score.c',
  c_args: tonal_args,
  dependencies: [glib, libm],
  version: '0.1.0',
  install: true)
tonal_dep = declare_dependency(link_with: tonal_lib, include_directories: include_directories('.'),
  dependencies: glib)
install_headers('tonal.h', subdir: 'tonal-1')
pkg.generate(tonal_lib, name: 'tonal-1', subdirs: 'tonal-1', requires: 'glib-2.0',
  description: 'Material 3 colour schemes from a seed colour or an image')

# about 35 s in an arm64 build under qemu, most of it the reference digests
test('tonal', executable('test-tonal', 'tests/test-tonal.c', dependencies: [tonal_dep, libm]), timeout: 300)

# vibeos-theme: computes the theme from a seed or a wallpaper and stores it in vibeos.Theme
executable('vibeos-theme', 'vibeos-theme.c',
  dependencies: [tonal_dep, libm, dependency('gio-2.0'), dependency('gdk-pixbuf-2.0')],
  install: true)
install_data('data/vibeos.Theme.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0/schemas')
import('gnome').post_install(glib_compile_schemas: true)
