project('vibeos-software', 'c', version: '0.1.1', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DSW_VERSION="@0@"'.format(meson.project_version()), language: 'c')

gnome = import('gnome')
gio = dependency('gio-unix-2.0', version: '>= 2.76')
soup = dependency('libsoup-3.0', version: '>= 3.0')
json = dependency('json-glib-1.0', version: '>= 1.6')
vibeos = dependency('vibeos-1')
datadir = get_option('prefix') / get_option('datadir')
src = include_directories('src')

# the store's API, its JSON, dpkg's list and the device: no widgets, so the tests link them alone
core = static_library('software-core', 'src/sw-api.c', 'src/sw-json.c', 'src/sw-dpkg.c', 'src/sw-device.c',
                      dependencies: [gio, soup, json, vibeos])

gtk = dependency('gtk4', version: '>= 4.14')
gtk_wayland = dependency('gtk4-wayland', required: false)
secret = dependency('libsecret-1')
spongy = dependency('spongy-gtk-1')
resources = gnome.compile_resources('software-resources', 'src/software.gresource.xml',
                                    source_dir: ['src', 'data'])
executable('vibeos-software', 'src/sw-main.c', 'src/sw-window.c', 'src/sw-widgets.c', 'src/sw-image.c',
           'src/sw-menu.c', 'src/sw-installer.c', 'src/sw-updates.c', 'src/sw-account.c', 'src/sw-featured.c',
           'src/sw-list.c', 'src/sw-app-page.c', 'src/sw-reviews.c', 'src/sw-search.c', 'src/sw-mine.c',
           'src/sw-account-pages.c', resources, link_with: core, install: true,
           dependencies: [gio, soup, json, vibeos, gtk, gtk_wayland, secret, spongy,
                          meson.get_compiler('c').find_library('m')])

service = configuration_data({'bindir': get_option('prefix') / get_option('bindir')})
configure_file(input: 'data/vibeos.Software.service.in', output: 'vibeos.Software.service',
               configuration: service, install_dir: datadir / 'dbus-1/services')
install_data('data/vibeos.Software.desktop', install_dir: datadir / 'applications')
install_data('data/vibeos.Software.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
# vibeos.Software: when to look for updates, light or dark, the recent searches
install_data('data/vibeos.Software.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)
# the schema compiled in the build directory, for running from it (GSETTINGS_SCHEMA_DIR=<build>)
custom_target('schemas', input: 'data/vibeos.Software.gschema.xml', output: 'gschemas.compiled',
              command: [find_program(dependency('gio-2.0').get_variable('glib_compile_schemas')), '--strict', '--targetdir',
                        meson.current_build_dir(), meson.current_source_dir() / 'data'], build_by_default: true)

test('api', executable('test-api', 'tests/test-api.c', include_directories: src, link_with: core,
                       dependencies: [gio, soup, json, vibeos]), timeout: 60)
