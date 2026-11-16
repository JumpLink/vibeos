project('vibeos-compass', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

deps = [dependency('gtk4', version: '>= 4.14'), dependency('libadwaita-1', version: '>= 1.5'),
        meson.get_compiler('c').find_library('m')]

executable('vibeos-compass', 'main.c', dependencies: deps, install: true)
install_data('data/vibeos.Compass.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/vibeos.Compass.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
