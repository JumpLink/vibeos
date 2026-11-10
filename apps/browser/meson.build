project('neo-browser', 'c', version: '2.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

# LineageOS's Jelly over WebKitGTK (spec/jelly.md): libneo draws the Material parts, libspongy scrolls the lists
gnome = import('gnome')
deps = [dependency('gtk4', version: '>= 4.14'), dependency('webkitgtk-6.0', version: '>= 2.46'),
        dependency('libsoup-3.0'), dependency('json-glib-1.0'), dependency('neo-1'), dependency('spongy-gtk-1'),
        meson.get_compiler('c').find_library('m')]

# EasyList for the first start (copied in beside the app: it is not in git)
datadir = get_option('prefix') / get_option('datadir') / 'neo-browser'
add_project_arguments('-DNB_DATADIR="@0@"'.format(datadir), language: 'c')

resources = gnome.compile_resources('browser-resources', 'src/browser.gresource.xml',
                                    source_dir: ['src', 'data/jelly-icons'])
executable('neo-browser', 'src/main.c', 'src/nb-app.c', 'src/nb-tab.c', 'src/nb-urlbar.c', 'src/nb-suggest.c',
           'src/nb-menu.c', 'src/nb-pages.c', 'src/nb-settings.c', 'src/nb-widgets.c', 'src/nb-navigator.c',
           'src/store.c', resources, dependencies: deps, install: true)
install_data('data/de.yesman.NeoBrowser.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/de.yesman.Browser.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
