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

test('api', executable('test-api', 'tests/test-api.c', include_directories: src, link_with: core,
                       dependencies: [gio, soup, json, vibeos]), timeout: 60)
