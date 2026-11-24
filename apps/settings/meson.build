project('vibeos-settings', 'c', version: '0.2', default_options: ['warning_level=2', 'c_std=gnu11'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')
add_project_arguments('-DNS_VERSION="@0@"'.format(meson.project_version()), language: 'c')
add_project_arguments('-DGETTEXT_PACKAGE="vibeos-settings"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')
# the keyring helper the pattern dialog runs, where this project installs it
add_project_arguments('-DNS_REKEY="@0@"'.format(get_option('prefix') / get_option('libexecdir') / 'vibeos-keyring-rekey'),
                      language: 'c')
# and the helper that sets the new password (through pkexec)
add_project_arguments('-DNS_SET_PASSWORD="@0@"'.format(get_option('prefix') / get_option('libexecdir') /
                                                       'vibeos-set-own-password'), language: 'c')

gnome = import('gnome')
i18n = import('i18n')
cc = meson.get_compiler('c')
deps = [dependency('gtk4', version: '>= 4.14'), dependency('vibeos-1'),
        # App Installer's sheet over Settings' window, for an uninstall (xdg-foreign)
        dependency('gtk4-wayland', required: false),
        dependency('json-glib-1.0'), dependency('goa-backend-1.0'), dependency('gio-unix-2.0'), dependency('gmodule-2.0'),
        # the colour swatches show what the theme engine makes of each seed
        dependency('tonal-1'),
        dependency('libnm'), dependency('libdrm'), dependency('libpulse-mainloop-glib'),
        # the account pages check a server the way GOA's dialogs do (libsoup), and hand Kerberos passwords to
        # goa-identity-service the way GOA does (gcr's secret exchange)
        dependency('libsoup-3.0'), dependency('gcr-4'),
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
  'src/ns-displays.c', 'src/ns-caps.c', 'src/ns-charge.c', 'src/ns-accounts.c', 'src/ns-accounts-net.c',
  'src/ns-perms.c', 'src/ns-buttons.c',
  resources,
  link_with: monitors,
  dependencies: deps,
  install: true)
test('monitors', executable('test-monitors', 'tests/test-monitors.c', link_with: monitors, dependencies: gio,
                            c_args: '-DTEST_DATA="@0@"'.format(meson.current_source_dir() / 'tests')))
i18n.merge_file(input: 'data/vibeos.Settings.desktop.in', output: 'vibeos.Settings.desktop', type: 'desktop',
                po_dir: 'po', install: true, install_dir: get_option('datadir') / 'applications')
install_data('data/vibeos.Settings.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
install_data('data/vibeos-keyring-rekey', install_dir: get_option('libexecdir'), install_mode: 'rwxr-xr-x')
# Set PIN / Set pattern: the helper that changes the user's own password after checking the current one, run through
# pkexec without a prompt for the active local session only (see src/vibeos-set-own-password.c), and its PAM service
# (in libpam's vendor directory: an administrator's /etc/pam.d/vibeos-set-own-password comes first)
executable('vibeos-set-own-password', 'src/vibeos-set-own-password.c', dependencies: cc.find_library('pam'),
           install: true, install_dir: get_option('libexecdir'))
policy = configuration_data()
policy.set('libexecdir', get_option('prefix') / get_option('libexecdir'))
configure_file(input: 'data/org.vibeos.set-own-password.policy.in', output: 'org.vibeos.set-own-password.policy',
               configuration: policy, install_dir: get_option('datadir') / 'polkit-1' / 'actions')
install_data('data/50-vibeos-set-own-password.rules', install_dir: get_option('datadir') / 'polkit-1' / 'rules.d')
install_data('data/vibeos-set-own-password.pam', rename: 'vibeos-set-own-password',
             install_dir: get_option('prefix') / 'lib' / 'pam.d')
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
# the page a panel's desktop file opens, where it is not the panel's own name: GNOME Shell opens "network" for VPN,
# wired and Bluetooth network settings (Network and VPN), the quick-settings airplane tile for Connections
panel_pages = {'network': 'vpn'}
foreach panel : gnome_panels
  panel_conf = configuration_data()
  panel_conf.set('panel', panel)
  panel_conf.set('page', panel_pages.get(panel, panel))
  panel_in = configure_file(input: 'data/gnome-panel.desktop.in', output: 'gnome-@0@-panel.desktop.in'.format(panel),
                            configuration: panel_conf)
  i18n.merge_file(input: panel_in, output: 'gnome-@0@-panel.desktop'.format(panel), type: 'desktop', po_dir: 'po',
                  install: true, install_dir: get_option('datadir') / 'applications')
endforeach
i18n.merge_file(input: 'data/org.gnome.Settings.desktop.in', output: 'org.gnome.Settings.desktop', type: 'desktop',
                po_dir: 'po', install: true, install_dir: get_option('datadir') / 'applications')
configure_file(input: 'data/org.gnome.Settings.service.in', output: 'org.gnome.Settings.service',
               configuration: service, install_dir: get_option('datadir') / 'dbus-1' / 'services')
install_data('data/gnome-control-center', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')

# the translations, and their checks
subdir('po')
