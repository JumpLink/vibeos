project('vibeos-clock', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

# glibc's first weekday (_NL_TIME_FIRST_WEEKDAY) is an enum item, not a macro, so no #ifdef sees it: whether the C
# library has it is found out by compiling, as GTK does
if meson.get_compiler('c').compiles('''#include <langinfo.h>
int main (void) { return *nl_langinfo (_NL_TIME_FIRST_WEEKDAY); }''', name: '_NL_TIME_FIRST_WEEKDAY')
  add_project_arguments('-DHAVE__NL_TIME_FIRST_WEEKDAY', language: 'c')
endif

gnome = import('gnome')
gio = dependency('gio-unix-2.0', version: '>= 2.76')
m = meson.get_compiler('c').find_library('m')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
i18n = import('i18n')
gettext_args = ['-DGETTEXT_PACKAGE="vibeos-clock"', '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir'))]

# dates and times in the user's language: libvibeos's vibeos-locale (GLib and ICU only), which the app has from
# libvibeos and the service and the tests, without GTK, build in
icu = [dependency('icu-i18n'), dependency('icu-uc')]
locale_inc = include_directories('../../libvibeos')
locale_src = files('../../libvibeos/vibeos-locale.c')

# the alarms, the timers and the stopwatch, and their rules: shared by the service, the app and the tests
models = static_library('models', 'src/alarm-model.c', 'src/alarm-engine.c', 'src/timer-model.c',
                        'src/stopwatch-model.c', 'src/elapsed.c', c_args: gettext_args, include_directories: locale_inc,
                        dependencies: [gio, icu])

# the service: alarms, timers and the stopwatch, with the app closed
executable('vibeos-clock-alarms', 'src/clock-service.c', 'src/alarm-service.c', 'src/timer-service.c',
           'src/stopwatch-service.c', 'src/alarm-klaxon.c', 'src/gst-start.c', 'src/alarm-notifications.c', 'src/clock-notifier.c',
           'src/phone.c', locale_src, link_with: models, install: true, c_args: gettext_args,
           include_directories: locale_inc,
           dependencies: [gio, m, icu, dependency('gstreamer-1.0'), dependency('libpulse-mainloop-glib')])

# the cities and their time zones (cities-data.h is made by tools/cities.py from DeskClock's cities.xml): the app's
# world clocks, the home clock and the cities picker, and the tests
cities = static_library('cities', 'src/cities.c', c_args: gettext_args, dependencies: gio)

# the app: its tabs, and the full-screen ringing alarm and expired timers (vibeos-clock --ringing)
resources = gnome.compile_resources('clock-resources', 'src/clock.gresource.xml', source_dir: ['src', 'data'])
executable('vibeos-clock', 'src/clock-main.c', 'src/clock-window.c', 'src/alarm-page.c', 'src/alarm-card.c',
           'src/clock-page.c', 'src/ringing-window.c', 'src/alarms-client.c', 'src/clock-format.c',
           'src/clock-tab.c', 'src/clock-draw.c', 'src/timers-client.c', 'src/stopwatch-client.c',
           'src/timer-page.c', 'src/timer-card.c', 'src/timer-setup.c', 'src/stopwatch-page.c',
           'src/expired-window.c', 'src/clock-navigator.c', 'src/clock-bar.c', 'src/clock-popup-menu.c',
           'src/clock-dialogs.c', 'src/analog-clock.c', 'src/city-picker.c', 'src/settings-page.c',
           'src/volume-slider.c', 'src/ringtone-picker.c', 'src/screensaver.c', resources,
           link_with: [models, cities], install: true,
           c_args: gettext_args,
           dependencies: [gio, m, dependency('gtk4', version: '>= 4.14'), dependency('vibeos-1'),
                          dependency('spongy-gtk-1')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/vibeos-clock-alarms.service.in', output: 'vibeos-clock-alarms.service',
               configuration: unit, install_dir: get_option('prefix') / 'lib/systemd/user')
configure_file(input: 'data/vibeos.Clock.Alarms.service.in', output: 'vibeos.Clock.Alarms.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
configure_file(input: 'data/vibeos.Clock.service.in', output: 'vibeos.Clock.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
foreach desktop : ['vibeos.Clock.desktop', 'vibeos.Clock.Ringing.desktop']
  i18n.merge_file(input: 'data' / desktop + '.in', output: desktop, type: 'desktop', po_dir: 'po', install: true,
                  install_dir: datadir / 'applications')
endforeach
install_data('data/vibeos.Clock.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/vibeos.Clock.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
install_data('data/sounds/alarm_expire.ogg', 'data/sounds/timer_expire.ogg', install_dir: datadir / 'vibeos-clock/sounds')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

# the translations, and their checks
subdir('po')

# in English: the tests compare the English texts
test_env = ['LANG=C.UTF-8', 'LANGUAGE=', 'LC_ALL=', 'LC_TIME=', 'LC_MESSAGES=']
foreach name : ['alarm-model', 'alarm-engine', 'timer-model']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', locale_src, link_with: models,
                        include_directories: locale_inc, dependencies: [gio, icu]), env: test_env)
endforeach
test('cities', executable('test-cities', 'tests/test-cities.c', link_with: cities, dependencies: gio), env: test_env)

# the service with a GStreamer whose start never finishes (a plugin scanner that sleeps for good): an alarm rings in
# the minute after the next, so the test takes up to two
test('service-gst-hang', find_program('python3'), timeout: 240, env: test_env,
     args: [files('tests/test-service-gst-hang.py'), meson.current_build_dir(), meson.current_source_dir()])
