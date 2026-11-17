project('vibeos-settings', 'c', version: '0.2', default_options: ['warning_level=2', 'c_std=gnu11'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')
add_project_arguments('-DNS_VERSION="@0@"'.format(meson.project_version()), language: 'c')
# the keyring helper the pattern dialog runs, where this project installs it
add_project_arguments('-DNS_REKEY="@0@"'.format(get_option('prefix') / get_option('libexecdir') / 'vibeos-keyring-rekey'),
                      language: 'c')

gnome = import('gnome')
cc = meson.get_compiler('c')
deps = [dependency('gtk4', version: '>= 4.14'), dependency('vibeos-1'),
        dependency('json-glib-1.0'), dependency('goa-backend-1.0'), dependency('gio-unix-2.0'), dependency('gmodule-2.0'),
        # the colour swatches show what the theme engine makes of each seed
        dependency('tonal-1'),
        dependency('libnm'), dependency('libdrm'), dependency('libpulse-mainloop-glib'),
        # the pattern dialog checks the current password the way the lock screen does
        cc.find_library('pam'), cc.find_library('m', required: false)]
# the lists scroll the way the launcher's drawer does (VibeOS's libspongy), where it is installed
spongy = dependency('spongy-gtk-1', required: false)
if spongy.found()
  deps += spongy
  add_project_arguments('-DHAVE_SPONGY', language: 'c')
endif
resources = gnome.compile_resources('ns-resources', 'src/vibeos-settings.gresource.xml', source_dir: 'src')

# the displays as mutter has them and the configurations Display asks it for: the app's and the tests'
gio = dependency('gio-2.0')
monitors = static_library('monitors', 'src/ns-monitors.c', dependencies: [gio, cc.find_library('m', required: false)])

executable('vibeos-settings',
  'src/main.c', 'src/ns-ui.c', 'src/ns-pages.c', 'src/ns-parallax.c', 'src/ns-net.c', 'src/ns-bt.c',
  'src/ns-sound.c', 'src/ns-apps.c', 'src/ns-shell.c', 'src/ns-pattern.c', 'src/ns-privacy.c', 'src/ns-system.c',
  'src/ns-displays.c', 'src/ns-caps.c', 'src/ns-charge.c',
  resources,
  link_with: monitors,
  dependencies: deps,
  install: true)
test('monitors', executable('test-monitors', 'tests/test-monitors.c', link_with: monitors, dependencies: gio,
                            c_args: '-DTEST_DATA="@0@"'.format(meson.current_source_dir() / 'tests')))
install_data('data/vibeos.Settings.desktop', install_dir: get_option('datadir') / 'applications')
install_data('data/vibeos.Settings.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
install_data('data/vibeos-keyring-rekey', install_dir: get_option('libexecdir'), install_mode: 'rwxr-xr-x')
# D-Bus activation (DBusActivatable=true): whatever opens a page through GApplication's "launch-panel" action,
# GNOME's way of opening a Settings panel, starts the app when it is not running
service = configuration_data()
service.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(input: 'data/vibeos.Settings.service.in', output: 'vibeos.Settings.service',
               configuration: service, install_dir: get_option('datadir') / 'dbus-1' / 'services')

# VibeOS Settings replaces GNOME Settings (gnome-control-center): GNOME's desktop files (the app's, and one per panel,
# which GNOME Shell's menus open), its name on the session bus (src/main.c answers it) and its command all open
# VibeOS Settings, at the panel's page
gnome_panels = ['about', 'applications', 'background', 'bluetooth', 'color', 'datetime', 'display', 'keyboard',
                'mouse', 'multitasking', 'network', 'notifications', 'online-accounts', 'power', 'printers', 'privacy',
                'region', 'search', 'sharing', 'sound', 'system', 'thunderbolt', 'universal-access', 'users', 'wacom',
                'wellbeing', 'wifi', 'wwan']
foreach panel : gnome_panels
  panel_conf = configuration_data()
  panel_conf.set('panel', panel)
  configure_file(input: 'data/gnome-panel.desktop.in', output: 'gnome-@0@-panel.desktop'.format(panel),
                 configuration: panel_conf, install_dir: get_option('datadir') / 'applications')
endforeach
install_data('data/org.gnome.Settings.desktop', install_dir: get_option('datadir') / 'applications')
configure_file(input: 'data/org.gnome.Settings.service.in', output: 'org.gnome.Settings.service',
               configuration: service, install_dir: get_option('datadir') / 'dbus-1' / 'services')
install_data('data/gnome-control-center', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
