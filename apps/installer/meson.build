project('vibeos-installer', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DGETTEXT_PACKAGE="vibeos-installer"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
                      '-DINST_VERSION="@0@"'.format(meson.project_version()), language: 'c')

gnome = import('gnome')
gio = dependency('gio-unix-2.0', version: '>= 2.76')
appstream = dependency('appstream', version: '>= 1.0')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
sysconfdir = get_option('sysconfdir')
src = include_directories('src')

# the packages, their versions, PackageKit and the rules: no GTK, so the tests link them alone
core = static_library('installer-core', 'src/inst-version.c', 'src/inst-deb.c', 'src/inst-dpkg.c',
                      'src/inst-demo.c', 'src/inst-pk.c', 'src/inst-catalog.c', 'src/inst-job.c',
                      dependencies: [gio, appstream])

gtk = dependency('gtk4', version: '>= 4.14')
gtk_wayland = dependency('gtk4-wayland', required: false)
resources = gnome.compile_resources('installer-resources', 'src/installer.gresource.xml',
                                    source_dir: ['src', 'data'])
executable('vibeos-installer', 'src/inst-main.c', 'src/inst-window.c', 'src/inst-view.c', 'src/inst-sheet.c',
           'src/inst-service.c', resources, link_with: core, install: true,
           dependencies: [gio, appstream, gtk, gtk_wayland, dependency('vibeos-1'), dependency('spongy-gtk-1')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/vibeos.Installer.service.in', output: 'vibeos.Installer.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/vibeos.Installer.desktop', install_dir: datadir / 'applications')
install_data('data/vibeos.Installer.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
# the interface other apps call, documented in it
install_data('data/vibeos.Installer1.xml', install_dir: datadir / 'dbus-1/interfaces')
# the default app for .deb files in the session (read before Debian's /usr/share/applications/gnome-mimeapps.list)
install_data('data/gnome-mimeapps.list', install_dir: get_option('prefix') / sysconfdir / 'xdg')
gnome.post_install(gtk_update_icon_cache: true, update_desktop_database: true)

test('inst', executable('test-inst', 'tests/test-inst.c', include_directories: src, link_with: core,
                        dependencies: [gio, appstream]), timeout: 120)
