# The vibeos GNOME Shell extension, installed system-wide (debian/: gnome-shell-extension-vibeos).
# Its own project, like libvibeos and the apps. Everything in this directory is the extension except the generators in
# tools/ (vd2js.py, l3icons.py: they read an Android source tree) and Python's caches.
project('gnome-shell-extension-vibeos', version: '0.1.0', meson_version: '>= 1.0')

uuid = 'shell@vibeos'
extdir = get_option('datadir') / 'gnome-shell' / 'extensions' / uuid
bindir = get_option('prefix') / get_option('bindir')

install_subdir('.', install_dir: extdir, strip_directory: true,
  exclude_directories: ['tools', 'tests', 'po', '__pycache__'],
  exclude_files: ['meson.build', 'home.js', 'appmem.py'])

# appmem.py runs as a program of its own (recents.js)
install_data('appmem.py', install_dir: extdir, install_mode: 'rwxr-xr-x')

# home.js starts the launcher by its path; a hand-installed launcher lives in /usr/local, the packaged one in bindir
configure_file(input: 'home.js', output: 'home.js', install_dir: extdir,
  command: ['sed', '-e', 's|/usr/local/bin/vibeos-launcher|' + bindir + '/vibeos-launcher|', '@INPUT@'], capture: true)

# the power key's handlers driven directly, never with a real key (tests/power-keys.sh); needs gjs and a D-Bus daemon
gjs = find_program('gjs', required: false)
dbus_run_session = find_program('dbus-run-session', required: false)
if gjs.found() and dbus_run_session.found()
  test('power-keys', find_program('tests/power-keys.sh'), args: [meson.current_source_dir()], timeout: 120)
endif

# Home's rule and a window asking for attention, with GNOME Shell's stand-ins (tests/home-attention.sh); needs gjs
if gjs.found()
  test('home-attention', find_program('tests/home-attention.sh'), args: [meson.current_source_dir()], timeout: 60)
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
