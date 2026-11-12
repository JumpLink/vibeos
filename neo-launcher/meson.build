deps = [
  dependency('vulkan'),
  dependency('wayland-client'),
  dependency('glib-2.0'),
  dependency('gio-unix-2.0'),
  dependency('gdk-pixbuf-2.0'),
  dependency('librsvg-2.0'),
  dependency('cairo'),
  dependency('freetype2'),
  dependency('harfbuzz'),
  dependency('fontconfig'),
  dependency('json-glib-1.0'),
  dependency('threads'),
  libm,
  spongy_dep,
]

# Wayland protocols: xdg-shell for the window, viewporter + fractional scale for the exact pixel size, presentation
# time for the frame clock
scanner = find_program('wayland-scanner')
wp = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')
protocols = [
  wp / 'stable/xdg-shell/xdg-shell.xml',
  wp / 'stable/viewporter/viewporter.xml',
  wp / 'stable/presentation-time/presentation-time.xml',
  wp / 'staging/fractional-scale/fractional-scale-v1.xml',
]
proto_src = []
foreach p : protocols
  name = p.split('/')[-1].replace('.xml', '')
  proto_src += custom_target(name + '-code', input: p, output: name + '-protocol.c',
                             command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  proto_src += custom_target(name + '-header', input: p, output: name + '-client-protocol.h',
                             command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
endforeach

executable('neo-launcher',
  'src/main.c', 'src/vk.c', 'src/gfx.c', 'src/wl.c', 'src/text.c', 'src/image.c', 'src/parallax.c', 'src/apps.c',
  'src/icons.c', 'src/model.c', 'src/tiles.c', 'src/home.c', 'src/drawer.c', 'src/nav.c',
  'src/popup.c', 'src/drag.c',
  proto_src,
  include_directories: include_directories('src', 'shaders'),
  dependencies: deps,
  install: true)

# the launcher's settings (de.yesman.neo), which the gnome-phone extension and Neo Settings read too
install_data('data/de.yesman.neo.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
# the first start's home screen (model.c); a device or distribution can put its own in /etc/xdg/neo-launcher
install_data('data/layout.json', install_dir: get_option('datadir') / 'neo-launcher')
gnome = import('gnome')
gnome.post_install(glib_compile_schemas: true)
