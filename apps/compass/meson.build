project('vibeos-compass', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DGETTEXT_PACKAGE="vibeos-compass"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')

# the window, its colours and its text are libvibeos's
deps = [dependency('gtk4', version: '>= 4.14'), dependency('vibeos-1'), meson.get_compiler('c').find_library('m')]

executable('vibeos-compass', 'main.c', dependencies: deps, install: true)
i18n = import('i18n')
i18n.merge_file(input: 'data/vibeos.Compass.desktop.in', output: 'vibeos.Compass.desktop', type: 'desktop',
                po_dir: 'po', install: true, install_dir: get_option('datadir') / 'applications')
# started by its D-Bus name, with the session's activation environment (a language changed in Settings)
configure_file(input: 'data/vibeos.Compass.service.in', output: 'vibeos.Compass.service',
               configuration: {'bindir': get_option('prefix') / get_option('bindir')},
               install_dir: get_option('datadir') / 'dbus-1/services')
install_data('data/vibeos.Compass.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')

# the translations, and their checks
subdir('po')
