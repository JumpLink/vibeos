project('gnome-phone', 'c', version: '0.1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])

add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')
cc = meson.get_compiler('c')
libm = cc.find_library('m')

subdir('libspongy')
subdir('neo-launcher')
subdir('boost')
subdir('auth')
