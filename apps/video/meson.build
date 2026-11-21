project('vibeos-video', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

# mpv plays (libmpv, its OpenGL renderer through epoxy); the window, its colours and its text are libvibeos's
deps = [dependency('gtk4', version: '>= 4.14'), dependency('vibeos-1'), dependency('mpv'), dependency('epoxy'),
        meson.get_compiler('c').find_library('m')]

executable('vibeos-video', 'main.c', dependencies: deps, install: true)
install_data('data/vibeos.Video.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/vibeos.Video.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
# the default app for videos (read before Debian's /usr/share/applications/gnome-mimeapps.list)
install_data('data/mimeapps.list', install_dir: get_option('sysconfdir') / 'xdg')
