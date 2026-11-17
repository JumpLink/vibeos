project('vibeos-weather', 'c', version: '0.1', default_options: ['warning_level=2', 'c_std=gnu11'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

gnome = import('gnome')
# the page scrolls the way the launcher's drawer does (VibeOS's libspongy: the fling, the stretch at the ends); its
# menu, the About sheet and Set City are libvibeos's, as in the other VibeOS apps
deps = [dependency('gtk4', version: '>= 4.14'), dependency('libadwaita-1', version: '>= 1.5'),
        dependency('json-glib-1.0'), dependency('vibeos-1'), dependency('spongy-gtk-1'),
        meson.get_compiler('c').find_library('m')]
resources = gnome.compile_resources('wx-resources', 'src/vibeos-weather.gresource.xml', source_dir: 'src')

executable('vibeos-weather', 'src/main.c', 'src/wx-menu.c', resources, dependencies: deps, install: true)
install_data('data/vibeos.Weather.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/vibeos.Weather.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
# vibeos.Weather: the city chosen in Set City
install_data('data/vibeos.Weather.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)
