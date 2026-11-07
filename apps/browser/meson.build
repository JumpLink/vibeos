project('neo-browser', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

deps = [dependency('gtk4', version: '>= 4.14'), dependency('libadwaita-1', version: '>= 1.5'),
        dependency('webkitgtk-6.0', version: '>= 2.46'), dependency('libsoup-3.0'), dependency('json-glib-1.0'),
        meson.get_compiler('c').find_library('m')]
# the lists scroll the way the launcher's drawer does (gnome-phone's libspongy), where it is installed
spongy = dependency('spongy-gtk-1', required: false)
if spongy.found()
  deps += spongy
  add_project_arguments('-DHAVE_SPONGY', language: 'c')
endif

# the toolbar's icons, and EasyList for the first start (copied in beside them: it is not in git)
datadir = get_option('prefix') / get_option('datadir') / 'neo-browser'
add_project_arguments('-DNB_DATADIR="@0@"'.format(datadir), '-DNB_ICONDIR="@0@"'.format(datadir / 'icons'),
                      language: 'c')

executable('neo-browser', 'src/browser.c', 'src/store.c', dependencies: deps, install: true)
install_subdir('data/icons', install_dir: datadir)
install_data('data/de.yesman.NeoBrowser.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/de.yesman.Browser.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
