project('vibeos-messaging', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      '-DGETTEXT_PACKAGE="vibeos-messaging"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')

gio = dependency('gio-unix-2.0', version: '>= 2.76')
ebook = dependency('libebook-1.2')
sqlite = dependency('sqlite3')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')

# the Phone's numbers, contacts, notifications and app opening are the Messaging's too, built from the Phone's
# sources, not copied
dialer = include_directories('../dialer/src')
store = static_library('store', 'src/sms-store.c', dependencies: [gio, sqlite])
phone = static_library('phone', '../dialer/src/phone-number.c', '../dialer/src/contacts.c', '../dialer/src/phone.c',
                       '../dialer/src/notifier.c', '../dialer/src/blocked-numbers.c', include_directories: dialer,
                       dependencies: [gio, ebook, sqlite],
                       c_args: ['-DNOTIFIER_APP_ID="vibeos.Messaging"', '-DNOTIFIER_APP_NAME="Messaging"'])

# the service: the messages with the app closed, taking them off the modem, sending, the notifications
executable('vibeos-messaging-sms', 'src/sms-service.c', 'src/sms-modem.c', link_with: [store, phone],
           include_directories: dialer, dependencies: [gio, ebook], install: true)

# the app: the conversation list, a conversation, the contact picker
gnome = import('gnome')
gtk = dependency('gtk4', version: '>= 4.14')
resources = gnome.compile_resources('messaging-resources', 'src/messaging.gresource.xml', source_dir: ['src', 'data'])
executable('vibeos-messaging', 'src/messaging-main.c', 'src/messaging-window.c', 'src/msg-navigator.c',
           'src/blocked-contacts.c', 'src/people-options.c', 'src/msg-settings.c', 'src/msg-sound.c',
           'src/conversation-list.c', 'src/conversation-view.c', 'src/contact-picker.c', 'src/msg-action-bar.c',
           'src/msg-add-contact.c', 'src/msg-avatar.c', 'src/msg-bubble.c', 'src/msg-dates.c', 'src/msg-image.c',
           'src/msg-people.c', 'src/msg-swipe.c', 'src/sms-client.c', 'src/sms-segments.c',
           '../dialer/src/popup-menu.c', '../dialer/src/contacts-app.c', '../dialer/src/app-action.c', resources,
           link_with: [store, phone],
           include_directories: dialer, install: true,
           c_args: ['-DSOUNDS_DIR="@0@"'.format(datadir / 'vibeos-messaging/sounds')],
           dependencies: [gio, ebook, gtk, dependency('vibeos-1'), dependency('spongy-gtk-1'),
                          dependency('gstreamer-1.0'), meson.get_compiler('c').find_library('m')])

unit = configuration_data({'bindir': bindir})
configure_file(input: 'data/vibeos-messaging-sms.service.in', output: 'vibeos-messaging-sms.service',
               configuration: unit, install_dir: get_option('prefix') / 'lib/systemd/user')
configure_file(input: 'data/vibeos.Messaging.Sms.service.in', output: 'vibeos.Messaging.Sms.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
configure_file(input: 'data/vibeos.Messaging.service.in', output: 'vibeos.Messaging.service',
               configuration: unit, install_dir: datadir / 'dbus-1/services')
install_data('data/vibeos.Messaging.desktop', install_dir: datadir / 'applications')
install_data('data/vibeos.Messaging.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/vibeos.Messaging.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
install_data('data/sounds/message_sent.wav', install_dir: datadir / 'vibeos-messaging/sounds')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

test('sms-store', executable('test-sms-store', 'tests/test-sms-store.c', link_with: store, dependencies: [gio]))
test('sms-segments', executable('test-sms-segments', 'tests/test-sms-segments.c', 'src/sms-segments.c',
                                dependencies: [gio]))
# the message flows end to end, against a mock modem and notification server on buses of their own
test('sms-flow', find_program('python3'), timeout: 120,
     args: [files('tests/test-sms.py'), meson.current_build_dir(), meson.current_source_dir()])
# a conversation's layout: date lines, runs, gaps, the date line's words (in UTC and English)
test('thread', executable('test-thread', 'tests/test-thread.c', 'src/msg-thread.c', dependencies: [gio]),
     env: ['TZ=UTC', 'LC_ALL=C.UTF-8'])

# made-up conversations and contacts for a test rig's private store and address book; not installed
executable('msg-seed', 'tests/msg-seed.c', include_directories: 'src', link_with: store, dependencies: [gio, ebook])
