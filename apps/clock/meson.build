project('neo-clock', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

gnome = import('gnome')
gio = dependency('gio-unix-2.0', version: '>= 2.76')
m = meson.get_compiler('c').find_library('m')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')

# the alarms and their rules, shared by the service, the app and the tests
alarms = static_library('alarms', 'src/alarm-model.c', 'src/alarm-engine.c', dependencies: gio)

# the alarm service
executable('neo-clock-alarms', 'src/alarm-service.c', 'src/alarm-klaxon.c', 'src/alarm-notifications.c',
           link_with: alarms, install: true,
           dependencies: [gio, m, dependency('gstreamer-1.0'), dependency('libpulse-mainloop-glib')])

# the app: its tabs, and the full-screen ringing alarm (neo-clock --ringing)
resources = gnome.compile_resources('clock-resources', 'src/clock.gresource.xml', source_dir: ['src', 'data'])
executable('neo-clock', 'src/clock-main.c', 'src/clock-window.c', 'src/alarm-page.c', 'src/alarm-card.c',
           'src/clock-page.c', 'src/ringing-window.c', 'src/alarms-client.c', 'src/clock-format.c', resources,
           link_with: alarms, install: true,
           c_args: ['-DGETTEXT_PACKAGE="neo-clock"', '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir'))],
           dependencies: [gio, m, dependency('gtk4', version: '>= 4.14'), dependency('neo-1'),
                          dependency('spongy-gtk-1')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/neo-clock-alarms.service.in', output: 'neo-clock-alarms.service',
               configuration: unit, install_dir: get_option('prefix') / 'lib/systemd/user')
configure_file(input: 'data/de.yesman.Clock.Alarms.service.in', output: 'de.yesman.Clock.Alarms.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
configure_file(input: 'data/de.yesman.Clock.service.in', output: 'de.yesman.Clock.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/de.yesman.Clock.desktop', 'data/de.yesman.Clock.Ringing.desktop',
             install_dir: datadir / 'applications')
install_data('data/de.yesman.Clock.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/de.yesman.Clock.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
install_data('data/sounds/alarm_expire.ogg', 'data/sounds/timer_expire.ogg', install_dir: datadir / 'neo-clock/sounds')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

foreach name : ['alarm-model', 'alarm-engine']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', link_with: alarms, dependencies: gio))
endforeach
