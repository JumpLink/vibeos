# The vibeos GNOME Shell extension, installed system-wide (debian/: gnome-shell-extension-vibeos).
# Its own project, like libvibeos and the apps. Everything in this directory is the extension except the generators in
# tools/ (vd2js.py, l3icons.py: they read an Android source tree) and Python's caches.
project('gnome-shell-extension-vibeos', version: '0.1.0', meson_version: '>= 1.0')

uuid = 'shell@vibeos'
extdir = get_option('datadir') / 'gnome-shell' / 'extensions' / uuid
bindir = get_option('prefix') / get_option('bindir')

install_subdir('.', install_dir: extdir, strip_directory: true,
  exclude_directories: ['tools', 'tests', 'po', '__pycache__'],
  exclude_files: ['meson.build', 'home.js', 'appmem.py', 'sounds/README'])

# appmem.py runs as a program of its own (recents.js)
install_data('appmem.py', install_dir: extdir, install_mode: 'rwxr-xr-x')

# home.js starts the launcher by its path; a hand-installed launcher lives in /usr/local, the packaged one in bindir
configure_file(input: 'home.js', output: 'home.js', install_dir: extdir,
  command: ['sed', '-e', 's|/usr/local/bin/vibeos-launcher|' + bindir + '/vibeos-launcher|', '@INPUT@'], capture: true)

# the power key's handlers driven directly, never with a real key (tests/power-keys.sh); needs gjs and a D-Bus daemon.
# Alone: its chords are 250 ms apart, which tests running beside it on a loaded machine can stretch
gjs = find_program('gjs', required: false)
dbus_run_session = find_program('dbus-run-session', required: false)
if gjs.found() and dbus_run_session.found()
  test('power-keys', find_program('tests/power-keys.sh'), args: [meson.current_source_dir()], timeout: 120,
    is_parallel: false)
endif

# the Bluetooth agent, trust after pairing and reconnection against a BlueZ stand-in (tests/bt-agent.sh); needs gjs and a
# D-Bus daemon
if gjs.found() and dbus_run_session.found()
  test('bt-agent', find_program('tests/bt-agent.sh'), args: [meson.current_source_dir()], timeout: 120)
endif

# the keyboard's click and the volume sound (sounds.js): the motor, the defaults, when they are silent, the throttle and
# the calls, against stand-ins on a bus of the test's own (tests/sounds.sh); needs gjs and a D-Bus daemon
glib_compile_schemas = find_program('glib-compile-schemas', required: false)
if gjs.found() and dbus_run_session.found() and glib_compile_schemas.found()
  test('sounds', find_program('tests/sounds.sh'), args: [meson.current_source_dir()], timeout: 120)
endif

# the click and the volume sound really played, through a PipeWire of the test's own with two null sinks, each recorded
# (tests/sounds-pipewire.sh); needs PipeWire, WirePlumber, pipewire-pulse, libcanberra-pulse and GSound, and is skipped
# without them. Alone: what it records and how long a play takes are what it checks
if gjs.found() and dbus_run_session.found()
  test('sounds-pipewire', find_program('tests/sounds-pipewire.sh'), args: [meson.current_source_dir()], timeout: 180,
    is_parallel: false)
endif

# what a Back button is called in an app's language, from catalogs it compiles (tests/back-names.sh); needs gjs and
# msgfmt. Its question asked of a real app over a real accessibility bus is tests/back-probe.sh, not run here (it
# starts a compositor).
msgfmt = find_program('msgfmt', required: false)
if gjs.found() and msgfmt.found()
  test('back-names', find_program('tests/back-names.sh'), args: [meson.current_source_dir()], timeout: 60)
endif

# Home's rule and a window asking for attention, with GNOME Shell's stand-ins (tests/home-attention.sh); needs gjs
if gjs.found()
  test('home-attention', find_program('tests/home-attention.sh'), args: [meson.current_source_dir()], timeout: 60)
endif

# the charging label's rule on fake power supply trees (tests/charge-power.sh); needs gjs
if gjs.found()
  test('charge-power', find_program('tests/charge-power.sh'), args: [meson.current_source_dir()], timeout: 60)
endif

# the shell's components kept to the session mode, the keyring's dialog off under the lock (tests/componentsync.sh)
if gjs.found()
  test('componentsync', find_program('tests/componentsync.sh'), args: [meson.current_source_dir()], timeout: 60)
endif

# the lock screen's picture of the wallpaper at rest: made by the launcher once and taken from the cache, a launcher
# that never ends ended after a timeout, and the first picture never held up by it (the boot splash's wait), against a
# stand-in launcher (tests/rest-picture.sh); needs gjs. About 5 s, and nearer 50 s under emulation, where its
# timeouts are stretched four times (VIBEOS_TEST_EMULATED)
if gjs.found()
  test('rest-picture', find_program('tests/rest-picture.sh'), args: [meson.current_source_dir()], timeout: 180)
endif

# every name a module uses is declared or imported (tests/undefined-names.sh); needs TypeScript's checker
# TypeScript 5 or later: tests/tsconfig.json's moduleResolution "bundler" is 5.0's (4.x refuses the configuration, and
# 6 no longer reads 4.x's "node"); an older checker leaves the test out
tsc = find_program('tsc', required: false, version: '>= 5.0')
if tsc.found()
  test('undefined-names', find_program('tests/undefined-names.sh'), args: [meson.current_source_dir(), tsc],
       timeout: 900)
endif

# the extension's words in the session's language: the vibeos-shell domain (metadata.json's gettext-domain, l10n.js)
i18n = import('i18n')
subdir('po')
