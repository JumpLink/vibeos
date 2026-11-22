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
brain_sources = ['src/dictate-brain.c', 'src/dictate-remote.c', 'src/dictate-device.c']

executable('vibeos-dictate', 'src/dictate-main.c', 'src/dictate-window.c', 'src/dictate-mic.c',
           'src/dictate-engine.c', 'src/dictate-models.c', 'src/dictate-moonshine.c', 'src/dictate-vosk.c',
           'src/dictate-vad.c', 'src/dictate-talk.c', 'src/dictate-voice.c', 'src/dictate-phonemes.c', brain_sources,
           resources, link_with: text, install: true,
           c_args: ['-DGETTEXT_PACKAGE="vibeos-dictate"',
                    '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
                    '-DLIBRARY_DIR="@0@"'.format(library_dir)],
           dependencies: [dependency('gtk4', version: '>= 4.14'), dependency('vibeos-1'), dependency('spongy-gtk-1'),
                          dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'), dependency('json-glib-1.0'),
                          cc.find_library('m'), cc.find_library('dl')] + agent_deps + remote_deps)

foreach id : ['vibeos.Dictate', 'vibeos.Agent']
  configure_file(input: 'data' / id + '.service.in', output: id + '.service',
                 configuration: configuration_data({'bindir': bindir}), install_dir: datadir / 'dbus-1/services')
  install_data('data' / id + '.desktop', install_dir: datadir / 'applications')
  install_data('data' / id + '.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
endforeach
install_data('data/vibeos.Dictate.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
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
           'src/dictate-voice.c', 'src/dictate-phonemes.c', brain_sources, include_directories: 'src',
           c_args: ['-DGETTEXT_PACKAGE="vibeos-dictate"', '-DLIBRARY_DIR="@0@"'.format(library_dir)],
           dependencies: [dependency('gio-2.0'), dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'),
                          dependency('json-glib-1.0'), cc.find_library('m'), cc.find_library('dl')] + agent_deps
                          + remote_deps)

# Agent's loop from typed text: the model's reply spoken on the speaker (tests/agent-say.c)
executable('agent-say', 'tests/agent-say.c', 'src/dictate-talk.c',
           'src/dictate-voice.c', 'src/dictate-phonemes.c', brain_sources, include_directories: 'src',
           c_args: ['-DGETTEXT_PACKAGE="vibeos-dictate"', '-DLIBRARY_DIR="@0@"'.format(library_dir)],
           dependencies: [dependency('gio-2.0'), dependency('gstreamer-1.0'), dependency('gstreamer-app-1.0'),
                          dependency('json-glib-1.0'), cc.find_library('m'), cc.find_library('dl')] + agent_deps
                          + remote_deps)

test('text', executable('test-text', 'tests/test-text.c', include_directories: 'src', link_with: text,
                        dependencies: dependency('glib-2.0')))
