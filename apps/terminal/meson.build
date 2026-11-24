project('vibeos-terminal', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-sign-compare',
                      '-DGETTEXT_PACKAGE="vibeos-terminal"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')

glib = dependency('glib-2.0', version: '>= 2.76')
m = meson.get_compiler('c').find_library('m')

# the private modes read from the program's output: no GTK, shared by the app and the tests
modes = static_library('modes', 'src/term-modes.c', dependencies: glib)

# the app
gnome = import('gnome')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
resources = gnome.compile_resources('terminal-resources', 'src/terminal.gresource.xml',
                                    source_dir: ['src', 'data'])
executable('vibeos-terminal', 'src/term-main.c', 'src/term-window.c', 'src/term-view.c', 'src/term-session.c',
           'src/term-toolbar.c', resources,
           link_with: modes, install: true,
           dependencies: [glib, m, dependency('gtk4', version: '>= 4.14'), dependency('vte-2.91-gtk4', version: '>= 0.78'),
                          dependency('vibeos-1'), dependency('spongy-1')])

configure_file(input: 'data/vibeos.Terminal.service.in', output: 'vibeos.Terminal.service',
               configuration: configuration_data({'bindir': bindir}), install_dir: datadir / 'dbus-1/services')
install_data('data/vibeos.Terminal.desktop', install_dir: datadir / 'applications')
install_data('data/vibeos.Terminal.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
gnome.post_install(gtk_update_icon_cache: true, update_desktop_database: true)

test('modes', executable('test-modes', 'tests/test-modes.c', include_directories: 'src', link_with: modes,
                         dependencies: glib))
