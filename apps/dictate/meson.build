project('vibeos-dictate', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

gnome = import('gnome')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
# the recognisers (Moonshine, Vosk) are not built here: vibeos-dictate loads their libraries at run time
# (src/dictate-recogniser.h), from this directory first
library_dir = get_option('prefix') / 'lib' / 'vibeos-dictate'

resources = gnome.compile_resources('dictate-resources', 'src/dictate.gresource.xml', source_dir: ['src', 'data'])
# the casing of what was heard: shared by the app and its test
text = static_library('text', 'src/dictate-text.c', dependencies: dependency('glib-2.0'))

cc = meson.get_compiler('c')
# Agent: the model (llama.cpp), the voices (onnxruntime, with espeak-ng for their phonemes) and the voice activity
# detector (onnxruntime) are linked, not loaded: they are the app's own, not a language package's
onnxruntime = dependency('libonnxruntime', required: false)
if not onnxruntime.found()
  onnxruntime = cc.find_library('onnxruntime')
endif
agent_deps = [dependency('llama'), onnxruntime, dependency('espeak-ng')]
# a model on a server instead (dictate-remote.c): the Agent's gateway, an OpenAI-style API
remote_deps = [dependency('libsoup-3.0'), dependency('json-glib-1.0')]
brain_sources = ['src/dictate-brain.c', 'src/dictate-remote.c', 'src/dictate-device.c', 'src/dictate-debuglog.c']
# what the model can use on the phone (dictate-tools.h): Contacts and Calendar through EDS, the call log and the
# messages from their apps' SQLite stores, the calls from the Phone's call service; and the fixed grammar that runs
# them when the model cannot be reached
tools_sources = ['src/dictate-tools.c', 'src/dictate-tools-phone.c', 'src/dictate-tools-device.c',
                 'src/dictate-actions.c', 'src/dictate-offline.c', 'src/dictate-calls.c', 'src/dictate-output.c']
# (and the sound server, asked where a reply would play for private mode)
tools_deps = [dependency('libebook-1.2'), dependency('libecal-2.0'), dependency('sqlite3'), dependency('libpulse'),
              dependency('libsoup-3.0'), dependency('json-glib-1.0'), cc.find_library('m')]
# a language's model and voice downloaded (dictate-download.c): libsoup for the files, libarchive for Vosk's zips
download_deps = [dependency('glib-2.0'), dependency('gio-2.0'), dependency('libsoup-3.0'), dependency('libarchive')]

# Vibe's screens: the conversation and its orb, the conversation kept, the lock screen's PIN asked for, the voice
# sample's recording and Settings' Voice match and Debug log pages, and the spoken lines in the language Vibe speaks
vibe_sources = ['src/dictate-stage.c', 'src/dictate-orb.c', 'src/dictate-history.c', 'src/dictate-owner.c',
                'src/dictate-l10n.c', 'src/dictate-enroll.c', 'src/dictate-voicematch-page.c',
                'src/dictate-debuglog-page.c', 'src/dictate-onboarding.c',
                'src/dictate-confirm-page.c']

executable('vibeos-dictate', 'src/dictate-main.c', 'src/dictate-window.c', 'src/dictate-mic.c',
           'src/dictate-engine.c', 'src/dictate-models.c', 'src/dictate-moonshine.c', 'src/dictate-vosk.c',
           'src/dictate-vad.c', 'src/dictate-talk.c', 'src/dictate-voicematch.c', 'src/dictate-voice.c',
           'src/dictate-phonemes.c', brain_sources,
           'src/dictate-download.c', 'src/dictate-download-dialog.c', 'src/dictate-pk.c', 'src/dictate-package.c',
           'src/dictate-access.c', 'src/dictate-settings.c', 'src/dictate-cues.c', vibe_sources, tools_sources, resources,
           link_with: text, install: true,
           c_args: ['-DGETTEXT_PACKAGE="vibeos-dictate"',
                    '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
                    '-DLIBRARY_DIR="@0@"'.format(library_dir)],
           dependencies: [dependency('gtk4', version: '>= 4.14'), dependency('vibeos-1'), dependency('spongy-gtk-1'),
                          dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'), dependency('json-glib-1.0'),
                          cc.find_library('m'), cc.find_library('dl')] + agent_deps + remote_deps + download_deps
                          + tools_deps)

foreach id : ['vibeos.Dictate', 'vibeos.Agent']
  configure_file(input: 'data' / id + '.service.in', output: id + '.service',
                 configuration: configuration_data({'bindir': bindir}), install_dir: datadir / 'dbus-1/services')
  install_data('data' / id + '.desktop', install_dir: datadir / 'applications')
  install_data('data' / id + '.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
endforeach
install_data('data/vibeos.Dictate.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
# Agent running with no window while Announce calls is on: the session starts it, and stops it when that goes off
install_data('data/vibeos.Agent-announce.desktop', install_dir: get_option('sysconfdir') / 'xdg/autostart')
# Vibe's introduction at the session's start, while it is still to come (the first-run key)
install_data('data/vibeos.Agent-intro.desktop', install_dir: get_option('sysconfdir') / 'xdg/autostart')
# Agent's state cues (dictate-cues.c; data/sounds/make-sounds.sh makes them)
install_data('data/sounds/agent-listening.wav', 'data/sounds/agent-working.wav', 'data/sounds/agent-done.wav',
             'data/sounds/agent-failed.wav', install_dir: datadir / 'vibeos-dictate/sounds')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

# the voices and the model, timed on the phone (not run by meson test: they need models)
executable('voice-bench', 'tests/voice-bench.c', 'src/dictate-voice.c', 'src/dictate-phonemes.c', include_directories: 'src',
           dependencies: [dependency('glib-2.0'), dependency('gio-2.0'), dependency('json-glib-1.0'),
                          cc.find_library('m'), onnxruntime, dependency('espeak-ng')])
executable('brain-bench', 'tests/brain-bench.c', brain_sources, include_directories: 'src',
           c_args: '-DGETTEXT_PACKAGE="vibeos-dictate"',
           dependencies: [dependency('glib-2.0'), dependency('gio-2.0'), dependency('llama')] + remote_deps)

# Agent's whole loop on recorded questions, timed (tests/talk-bench.c)
executable('talk-bench', 'tests/talk-bench.c', 'src/dictate-engine.c', 'src/dictate-models.c',
           'src/dictate-moonshine.c', 'src/dictate-vosk.c', 'src/dictate-vad.c', 'src/dictate-talk.c',
           'src/dictate-voicematch.c', 'src/dictate-access.c', 'src/dictate-voice.c', 'src/dictate-phonemes.c',
           brain_sources, tools_sources,
           include_directories: 'src',
           c_args: ['-DGETTEXT_PACKAGE="vibeos-dictate"', '-DLIBRARY_DIR="@0@"'.format(library_dir)],
           dependencies: [dependency('gio-2.0'), dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'),
                          dependency('json-glib-1.0'), cc.find_library('m'), cc.find_library('dl')] + agent_deps
                          + remote_deps + tools_deps)

# Agent's loop from typed text: the model's reply spoken on the speaker (tests/agent-say.c)
executable('agent-say', 'tests/agent-say.c', 'src/dictate-talk.c', 'src/dictate-voicematch.c',
           'src/dictate-access.c', 'src/dictate-voice.c', 'src/dictate-phonemes.c', brain_sources, tools_sources,
           include_directories: 'src',
           c_args: ['-DGETTEXT_PACKAGE="vibeos-dictate"', '-DLIBRARY_DIR="@0@"'.format(library_dir)],
           dependencies: [dependency('gio-2.0'), dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'),
                          dependency('json-glib-1.0'), cc.find_library('m'), cc.find_library('dl')] + agent_deps
                          + remote_deps + tools_deps)

# the tools' test set: questions in a private copy of the phone's data (fixtures made by it, the phone's services
# stood in for on a private bus), each answer scored (tests/agent-eval.c, tests/agent-eval/)
executable('agent-eval', 'tests/agent-eval.c', 'tests/agent-eval-mock.c', 'src/dictate-talk.c',
           'tests/agent-eval-voice.c', 'src/dictate-access.c', 'src/dictate-voice.c', 'src/dictate-phonemes.c',
           brain_sources, tools_sources,
           include_directories: 'src',
           c_args: ['-DGETTEXT_PACKAGE="vibeos-dictate"', '-DLIBRARY_DIR="@0@"'.format(library_dir)],
           dependencies: [dependency('gio-2.0'), dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'),
                          dependency('json-glib-1.0'), cc.find_library('m'), cc.find_library('dl')] + agent_deps
                          + remote_deps + tools_deps)

test('text', executable('test-text', 'tests/test-text.c', include_directories: 'src', link_with: text,
                        dependencies: dependency('glib-2.0')))
# what a download unpacks and removes (archives made by the test, nothing fetched), and the table of packs
test('download', executable('test-download', 'tests/test-download.c', 'src/dictate-download.c',
                            include_directories: 'src', c_args: '-DGETTEXT_PACKAGE="vibeos-dictate"',
                            dependencies: download_deps))

# what Agent is told it can use, from the settings' switches (the schema compiled here, a memory backend)
schemas = custom_target('schemas', input: 'data/vibeos.Dictate.gschema.xml', output: 'gschemas.compiled',
                        command: [find_program('glib-compile-schemas'), '--strict', '--targetdir=@OUTDIR@',
                                  meson.current_source_dir() / 'data'])
test('access', executable('test-access', 'tests/test-access.c', 'src/dictate-access.c', include_directories: 'src',
                          c_args: '-DGETTEXT_PACKAGE="vibeos-dictate"', dependencies: dependency('gio-2.0')),
     depends: schemas, env: ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir(), 'GSETTINGS_BACKEND=memory'])

# a switch that is off takes its area's tools away from the model, and a call to one anyway is refused
test('tools', executable('test-tools', 'tests/test-tools.c', 'src/dictate-access.c', 'src/dictate-device.c',
                         tools_sources,
                         include_directories: 'src', c_args: '-DGETTEXT_PACKAGE="vibeos-dictate"',
                         dependencies: [dependency('gio-2.0')] + tools_deps))

# what Agent says when a call rings, and when it may (the settings in memory)
test('calls', executable('test-calls', 'tests/test-calls.c', 'src/dictate-calls.c', include_directories: 'src',
                         dependencies: [dependency('gio-2.0'), dependency('json-glib-1.0')]),
     depends: schemas, env: ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir(), 'GSETTINGS_BACKEND=memory'])

# the debug log: a turn written whole and only the user's, the gateway's key never in it, the newest kept (each test
# in a private data directory)
test('debuglog', executable('test-debuglog', 'tests/test-debuglog.c', brain_sources, include_directories: 'src',
                            c_args: '-DGETTEXT_PACKAGE="vibeos-dictate"',
                            dependencies: [dependency('glib-2.0'), dependency('gio-2.0'), dependency('llama')]
                                          + remote_deps))

# the offline grammar: each command in each language to its tool, and the replies from made-up results
test('offline', executable('test-offline', 'tests/test-offline.c', 'src/dictate-offline.c', include_directories: 'src',
                           dependencies: [dependency('glib-2.0'), dependency('json-glib-1.0')]))

# a language's packs fetched for real, as the app fetches them (not run by meson test: it needs the network)
executable('download-try', 'tests/download-try.c', 'src/dictate-download.c', 'src/dictate-models.c',
           'src/dictate-pk.c', 'src/dictate-package.c',
           include_directories: 'src', c_args: '-DGETTEXT_PACKAGE="vibeos-dictate"', dependencies: download_deps)
