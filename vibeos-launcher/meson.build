# the renderer's two backends (src/gfx.h): Vulkan, and OpenGL ES 3.0 on EGL for a GPU without a Vulkan driver
deps = [
  dependency('vulkan'),
  dependency('egl'),
  dependency('glesv2'),
  dependency('wayland-egl'),
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
  dependency('xkbcommon'),
  dependency('threads'),
  # the home clock's date in the user's language (libvibeos's vibeos-locale.c, built in)
  dependency('icu-i18n'),
  dependency('icu-uc'),
  libm,
  spongy_dep,
  legacy_ids_dep,
]

# Wayland protocols: xdg-shell for the window, viewporter + fractional scale for the exact pixel size, presentation
# time for the frame clock, text input for the drawer's search (the on-screen keyboard)
scanner = find_program('wayland-scanner')
wp = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')
protocols = [
  wp / 'stable/xdg-shell/xdg-shell.xml',
  wp / 'stable/viewporter/viewporter.xml',
  wp / 'stable/presentation-time/presentation-time.xml',
  wp / 'staging/fractional-scale/fractional-scale-v1.xml',
  wp / 'unstable/text-input/text-input-unstable-v3.xml',
]
proto_src = []
foreach p : protocols
  name = p.split('/')[-1].replace('.xml', '')
  proto_src += custom_target(name + '-code', input: p, output: name + '-protocol.c',
                             command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  proto_src += custom_target(name + '-header', input: p, output: name + '-client-protocol.h',
                             command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
endforeach

executable('vibeos-launcher',
  'src/main.c', 'src/vk.c', 'src/gfx.c', 'src/gfx-vk.c', 'src/gfx-gl.c', 'src/wl.c', 'src/text.c', 'src/image.c',
  'src/parallax.c', 'src/apps.c', 'src/icons.c', 'src/model.c', 'src/tiles.c', 'src/home.c', 'src/drawer.c',
  'src/nav.c', 'src/popup.c', 'src/drag.c', 'src/pieces.c', '../libvibeos/vibeos-locale.c',
  proto_src,
  include_directories: include_directories('src', 'shaders', '../libvibeos'),
  c_args: ['-DMIGRATE_IDS="@0@"'.format(migrate_ids_path), '-DGETTEXT_PACKAGE="vibeos-launcher"',
           '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir'))],
  dependencies: deps,
  install: true)

# the launcher's settings (vibeos.Shell), which the vibeos extension and VibeOS Settings read too
install_data('data/vibeos.Shell.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
# the first start's home screen (model.c); a device or distribution can put its own in /etc/xdg/vibeos-launcher
install_data('data/layout.json', install_dir: get_option('datadir') / 'vibeos-launcher')
# the icon packs: VibeOS (tools/iconpack-vibeos makes it), packaged apart as vibeos-iconpack-vibeos with its default
# override, and gnome-free (tools/iconpack-gnome-free; called Round before VibeOS), packaged as vibeos-iconpack-gnome-free
install_subdir('iconpacks/vibeos', install_dir: get_option('datadir') / 'vibeos-launcher' / 'iconpacks')
install_subdir('iconpacks/gnome-free', install_dir: get_option('datadir') / 'vibeos-launcher' / 'iconpacks')
install_data('iconpacks/95_vibeos-iconpack-vibeos.gschema.override',
             install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
# the NASA solar-system parallax set (tools/parallax-solar-system makes it), packaged apart as vibeos-parallax-solar-system
install_subdir('parallax/solar-system', install_dir: get_option('datadir') / 'vibeos-launcher' / 'parallax')
install_data('parallax/95_vibeos-parallax-solar-system.gschema.override',
             install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
install_data('parallax/vibeos-parallax-solar-system.xml', install_dir: get_option('datadir') / 'gnome-background-properties')

# the 0.1.7 renaming as the launcher sees it: old desktop ids in layouts and in hidden-from-home read as the new ones
test('legacy-ids', executable('test-legacy-ids', 'tests/test-legacy-ids.c', 'src/model.c', 'src/apps.c',
                              include_directories: include_directories('src'),
                              dependencies: [dependency('gio-unix-2.0'), dependency('json-glib-1.0'), legacy_ids_dep]))

# the parts of a parallax layer it draws: all it shows, nothing twice
test('pieces', executable('test-pieces', 'tests/test-pieces.c', 'src/pieces.c',
                          include_directories: include_directories('src'),
                          dependencies: [dependency('glib-2.0'), libm]))

gnome = import('gnome')
gnome.post_install(glib_compile_schemas: true)

# the launcher's translations, and their checks
subdir('po')
