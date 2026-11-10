project('neo-files', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DGETTEXT_PACKAGE="neo-files"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
                      '-DFM_VERSION="@0@"'.format(meson.project_version()), language: 'c')

gnome = import('gnome')
# g_sort_array (stable sorting) is 2.82
gio = dependency('gio-unix-2.0', version: '>= 2.82')
m = meson.get_compiler('c').find_library('m')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
src = include_directories('src')

# the listings, the remotes, the locations and what the app remembers: no GTK, so the tests link them alone
core = static_library('files-core', 'src/fm-item.c', 'src/fm-remotes.c', 'src/fm-state.c', 'src/fm-util.c',
                      'src/fm-places.c', dependencies: [gio])

# a removed remote's password goes from the keyring too (the only keyring call the app makes); without libsecret
# GVfs's saved password stays behind
secret = dependency('libsecret-1', required: false)
config = configuration_data()
config.set10('HAVE_LIBSECRET', secret.found())
configure_file(output: 'fm-config.h', configuration: config)

gtk = dependency('gtk4', version: '>= 4.14')
resources = gnome.compile_resources('files-resources', 'src/files.gresource.xml', source_dir: ['src', 'data'])
executable('neo-files', 'src/fm-main.c', 'src/fm-window.c', 'src/fm-ui.c', 'src/fm-icon.c', 'src/fm-dialogs.c',
           'src/fm-list.c', 'src/fm-browser.c', 'src/fm-drawer.c', 'src/fm-home.c', 'src/fm-remote-page.c',
           'src/fm-mount.c', 'src/fm-jobs.c', 'src/fm-settings.c', 'src/fm-share.c', resources,
           link_with: core, install: true,
           dependencies: [gio, m, gtk, secret, dependency('gdk-pixbuf-2.0'), dependency('neo-1'),
                          dependency('spongy-gtk-1')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/de.yesman.Files.service.in', output: 'de.yesman.Files.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/de.yesman.Files.desktop', install_dir: datadir / 'applications')
install_data('data/de.yesman.Files.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/de.yesman.Files.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

foreach name : ['fm-item', 'fm-remotes', 'fm-state', 'fm-util', 'fm-places']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', include_directories: src, link_with: core,
                        dependencies: [gio]))
endforeach
