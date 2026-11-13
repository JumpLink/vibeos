project('gnome-phone', 'c', version: '0.1.0', license: 'MIT', license_files: 'LICENSE', meson_version: '>= 1.1',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])

add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')
cc = meson.get_compiler('c')
libm = cc.find_library('m')

subdir('libspongy')
subdir('migrate-ids')
subdir('neo-launcher')
subdir('boost')
subdir('auth')
subdir('wakeup')
