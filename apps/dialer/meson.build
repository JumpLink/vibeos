project('vibeos-dialer', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DGETTEXT_PACKAGE="vibeos-dialer"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')

gnome = import('gnome')
gio = dependency('gio-unix-2.0', version: '>= 2.76')
ebook = dependency('libebook-1.2')
m = meson.get_compiler('c').find_library('m')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')

# numbers, the call log and the contacts: shared by the service, the app and the tests
common = static_library('common', 'src/phone-number.c', 'src/call-log.c', 'src/contacts.c', 'src/blocked-numbers.c',
                        'src/call-recording.c', dependencies: [gio, ebook, dependency('sqlite3')])

# what the sound card's UCM says about recording a call: the app's Settings and the service
alsa = dependency('alsa')
call_audio_ucm = static_library('call-audio-ucm', 'src/call-audio-ucm.c', dependencies: [gio, alsa])

# the service: the calls with the app closed, ringing, the call log, the notifications
executable('vibeos-dialer-calls', 'src/calls-service.c', 'src/calls.c', 'src/modem.c', 'src/ringer.c',
           'src/call-notifications.c', 'src/notifier.c', 'src/phone.c', 'src/call-recorder.c',
           link_with: [common, call_audio_ucm], install: true,
           dependencies: [gio, ebook, m, dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'),
                          dependency('libpulse-mainloop-glib')])

# the app: its tabs and dialpad, and the in-call screen (vibeos-dialer --incall)
gtk = dependency('gtk4', version: '>= 4.14')
resources = gnome.compile_resources('dialer-resources', 'src/dialer.gresource.xml', source_dir: ['src', 'data'])
executable('vibeos-dialer', 'src/dialer-main.c', 'src/dialer-window.c', 'src/dialpad-sheet.c', 'src/dialpad.c',
           'src/search-bar.c', 'src/search-page.c', 'src/nav-bar.c', 'src/favorites-page.c', 'src/recents-page.c',
           'src/contacts-page.c', 'src/contacts-app.c', 'src/app-action.c', 'src/call-history.c',
           'src/call-details.c', 'src/call-stats.c', 'src/dialer-settings.c', 'src/popup-menu.c', 'src/empty-view.c',
           'src/call-dates.c', 'src/text.c', 'src/incall-window.c', 'src/incall-view.c', 'src/incall-button.c',
           'src/answer-view.c', 'src/avatar.c', 'src/caller.c', 'src/calls-client.c', 'src/phone.c', resources,
           link_with: [common, call_audio_ucm], install: true,
           dependencies: [gio, ebook, m, gtk, dependency('vibeos-1'), dependency('spongy-gtk-1')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/vibeos-dialer-calls.service.in', output: 'vibeos-dialer-calls.service',
               configuration: unit, install_dir: get_option('prefix') / 'lib/systemd/user')
configure_file(input: 'data/vibeos.Dialer.Calls.service.in', output: 'vibeos.Dialer.Calls.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
configure_file(input: 'data/vibeos.Dialer.service.in', output: 'vibeos.Dialer.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/vibeos.Dialer.desktop', 'data/vibeos.Dialer.InCall.desktop',
             install_dir: datadir / 'applications')
install_data('data/vibeos.Dialer.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/vibeos.Dialer.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
# "This call is now being recorded" in each of VibeOS's languages (data/sounds/make-sounds.sh: how they were made)
announcements = []
foreach lang : ['de', 'en', 'es', 'fr', 'it', 'nl', 'ru']
  announcements += 'data/sounds/call-recording-' + lang + '.opus'
endforeach
install_data(announcements, install_dir: datadir / 'vibeos-dialer/sounds')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

foreach name : ['call-log', 'phone-number', 'blocked-numbers']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', link_with: common,
                        dependencies: [gio, ebook, dependency('sqlite3')]))
endforeach

# the settings' defaults, from the schema compiled into the build directory
test_schemas = custom_target('test-schemas', input: 'data/vibeos.Dialer.gschema.xml', output: 'gschemas.compiled',
                             build_by_default: true,
                             command: [find_program('glib-compile-schemas'), '--strict', '--targetdir', '@OUTDIR@',
                                       meson.current_source_dir() / 'data'])
test('call-recording', executable('test-call-recording', 'tests/test-call-recording.c', link_with: common,
                                  dependencies: [gio, ebook, dependency('sqlite3')]),
     depends: test_schemas,
     env: ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir(), 'GSETTINGS_BACKEND=memory', 'LC_ALL=C.UTF-8'])
# against the UCM configurations for no card in tests/ucm2
test('call-audio-ucm', executable('test-call-audio-ucm', 'tests/test-call-audio-ucm.c', link_with: call_audio_ucm,
                                  dependencies: [gio, alsa]),
     env: ['ALSA_CONFIG_UCM2=' + meson.current_source_dir() / 'tests/ucm2'])

# the call flows end to end, against a mock modem, shell and notification server on buses of their own
test('calls-flow', find_program('python3'), timeout: 180,
     args: [files('tests/test-calls.py'), meson.current_build_dir(), meson.current_source_dir()])
