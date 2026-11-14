project('neo-dictate', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

gnome = import('gnome')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
# the recognisers (Moonshine, Vosk) are not built here: neo-dictate loads their libraries at run time
# (src/dictate-recogniser.h), from this directory first
library_dir = get_option('prefix') / 'lib' / 'neo-dictate'

resources = gnome.compile_resources('dictate-resources', 'src/dictate.gresource.xml', source_dir: ['src', 'data'])
# the casing of what was heard: shared by the app and its test
text = static_library('text', 'src/dictate-text.c', dependencies: dependency('glib-2.0'))

executable('neo-dictate', 'src/dictate-main.c', 'src/dictate-window.c', 'src/dictate-mic.c',
           'src/dictate-engine.c', 'src/dictate-models.c', 'src/dictate-moonshine.c', 'src/dictate-vosk.c',
           resources, link_with: text, install: true,
           c_args: ['-DGETTEXT_PACKAGE="neo-dictate"',
                    '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
                    '-DLIBRARY_DIR="@0@"'.format(library_dir)],
           dependencies: [dependency('gtk4', version: '>= 4.14'), dependency('neo-1'), dependency('spongy-gtk-1'),
                          dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'), dependency('json-glib-1.0'),
                          meson.get_compiler('c').find_library('m'), meson.get_compiler('c').find_library('dl')])

configure_file(input: 'data/neo.Dictate.service.in', output: 'neo.Dictate.service',
               configuration: configuration_data({'bindir': bindir}), install_dir: datadir / 'dbus-1/services')
install_data('data/neo.Dictate.desktop', install_dir: datadir / 'applications')
install_data('data/neo.Dictate.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/neo.Dictate.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

test('text', executable('test-text', 'tests/test-text.c', include_directories: 'src', link_with: text,
                        dependencies: dependency('glib-2.0')))
