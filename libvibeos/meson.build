# libvibeos: the phone apps' shared widgets (Material 3's look, dark), on GTK 4 and libspongy.
# Its own project, like the apps that use it: build and install libspongy (the top-level project) first.
project('libvibeos', 'c', version: '0.1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')
# libvibeos's own strings, in their gettext domain (vibeos-i18n.c binds it)
add_project_arguments('-DGETTEXT_PACKAGE="libvibeos-1"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')

libm = meson.get_compiler('c').find_library('m')
gtk4 = dependency('gtk4', version: '>= 4.14')
spongy_dep = dependency('spongy-1')
spongy_gtk_dep = dependency('spongy-gtk-1')
tonal_dep = dependency('tonal-1')
# dates in the user's language (vibeos-locale.c)
icu_dep = [dependency('icu-i18n'), dependency('icu-uc')]

gnome = import('gnome')
fs = import('fs')
i18n = import('i18n')

# The VibeOS release the About sheet shows: the newest entry of debian/changelog, the one file a release changes,
# without its epoch ("vibeos (1:0.1.1) unstable; ..." gives 0.1.1). Meson configures again when the file changes.
vibeos_release = ''
changelog = '..' / 'debian' / 'changelog'
if fs.is_file(changelog)
  vibeos_release = fs.read(changelog).split('\n')[0].split('(')[1].split(')')[0].split(':')[-1]
endif

vibeos_resources = gnome.compile_resources('vibeos-resources', 'vibeos.gresource.xml', c_name: 'vibeos')

vibeos_headers = files(
  'vibeos.h',
  'vibeos-style.h',
  'vibeos-anim.h',
  'vibeos-ripple.h',
  'vibeos-pressable.h',
  'vibeos-switch.h',
  'vibeos-nav-bar.h',
  'vibeos-snackbar.h',
  'vibeos-dialog.h',
  'vibeos-time-picker.h',
  'vibeos-locale.h',
  'vibeos-layout.h',
  'vibeos-page-stack.h',
  'vibeos-nav-rail.h',
  'vibeos-list-detail.h',
  'vibeos-fab-column.h',
  'vibeos-list-row.h',
  'vibeos-card-list.h',
  'vibeos-top-bar.h',
  'vibeos-search.h',
  'vibeos-sheet.h',
  'vibeos-about.h',
  'vibeos-menu.h',
  'vibeos-text.h',
  'vibeos-original-colors.h',
  'vibeos-extended-fab.h',
  'vibeos-clamp.h',
  'vibeos-fit-label.h',
)
# VibeosUnifiedPush, push messages (vibeos-unified-push.h)
subdir('unifiedpush')
vibeos_lib = shared_library('vibeos-1',
  'vibeos-style.c',
  'vibeos-anim.c',
  'vibeos-ripple.c',
  'vibeos-pressable.c',
  'vibeos-switch.c',
  'vibeos-clamp.c',
  'vibeos-fit-label.c',
  'vibeos-nav-bar.c',
  'vibeos-snackbar.c',
  'vibeos-dialog.c',
  'vibeos-time-picker.c',
  'vibeos-locale.c',
  'vibeos-i18n.c',
  'vibeos-layout.c',
  'vibeos-page-stack.c',
  'vibeos-nav-rail.c',
  'vibeos-list-detail.c',
  'vibeos-fab-column.c',
  'vibeos-list-row.c',
  'vibeos-card-list.c',
  'vibeos-top-bar.c',
  'vibeos-search.c',
  'vibeos-sheet.c',
  'vibeos-about.c',
  'vibeos-menu.c',
  'vibeos-dump.c',
  'vibeos-text.c',
  'vibeos-text-handles.c',
  'vibeos-original-colors.c',
  'vibeos-extended-fab.c',
  vibeos_resources,
  c_args: ['-DVIBEOS_RELEASE="@0@"'.format(vibeos_release)],
  link_whole: vibeos_unified_push_lib,
  dependencies: [gtk4, libm, spongy_dep, spongy_gtk_dep, tonal_dep, icu_dep],
  version: '0.1.0',
  install: true)
vibeos_dep = declare_dependency(link_with: vibeos_lib, include_directories: include_directories('.'),
  dependencies: [gtk4, spongy_dep, spongy_gtk_dep])
install_headers(vibeos_headers, subdir: 'vibeos-1')
import('pkgconfig').generate(vibeos_lib, name: 'vibeos-1', subdirs: 'vibeos-1', requires: ['gtk4', 'spongy-1', 'spongy-gtk-1'],
  description: 'The phone apps\' Material 3 widgets')

# Roboto as Android ships it (variable: weight 100-900, width 75-100), under its own family name so that the
# system's static Roboto cut can never stand in for it
install_data('fonts/VibeosRoboto.ttf', install_dir: get_option('datadir') / 'fonts' / 'vibeos')

# the theme without a display: its colours from vibeos.Theme, light and dark, and without the schema; dates in each
# language
subdir('tests')

# the translations of libvibeos's own strings, and their checks
subdir('po')

# the widget gallery, for trying the widgets on a desktop or a headless display (not installed)
subdir('demo')

# vibeos.Interface: Bold text and Display size (the font size is GNOME's own text-scaling-factor)
install_data('data/vibeos.Interface.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true)
