# vibeos-sandbox: the sandbox the apps installed from Software run in (bubblewrap, xdg-dbus-proxy and the portals'
# permission store), the user units that keep them there, and WirePlumber's part (the microphone and the cameras).
gio = dependency('gio-2.0')
gio_unix = dependency('gio-unix-2.0')
sqlite = dependency('sqlite3')

sandbox_core = static_library('vibeos-sandbox-core',
  'src/vs-perms.c', 'src/vs-bwrap.c', 'src/vs-exec.c', 'src/vs-registry.c', 'src/vs-run.c',
  dependencies: [gio, gio_unix, sqlite])

executable('vibeos-sandbox', 'src/vs-main.c', link_with: sandbox_core, dependencies: [gio, gio_unix],
           install: true)
sandbox_path = get_option('prefix') / get_option('bindir') / 'vibeos-sandbox'

user_units = get_option('prefix') / 'lib' / 'systemd' / 'user'
configure_file(input: 'data/vibeos-sandbox-sync.service.in', output: 'vibeos-sandbox-sync.service',
               configuration: {'path': sandbox_path}, install_dir: user_units)
configure_file(input: 'data/vibeos-sandbox-follow.service.in', output: 'vibeos-sandbox-follow.service',
               configuration: {'path': sandbox_path}, install_dir: user_units)
install_data('data/vibeos-sandbox-sync.path', install_dir: user_units)

wp_dir = get_option('prefix') / get_option('datadir') / 'wireplumber'
install_data('data/sandbox-access.lua', install_dir: wp_dir / 'scripts' / 'vibeos')
install_data('data/60-vibeos-sandbox.conf', install_dir: wp_dir / 'wireplumber.conf.d')
install_data('data/60-vibeos-sandbox-pulse.conf', rename: '60-vibeos-sandbox.conf',
             install_dir: get_option('prefix') / get_option('datadir') / 'pipewire' / 'pipewire-pulse.conf.d')

test('sandbox', executable('test-sandbox', 'tests/test-sandbox.c', link_with: sandbox_core,
                           include_directories: include_directories('src'),
                           dependencies: [gio, gio_unix, sqlite]),
     env: {'G_DEBUG': 'fatal-criticals'})
lua = find_program('lua5.4', required: false)
if lua.found()
  test('sandbox-wireplumber', lua, args: [files('tests/test-wp-access.lua'), files('data/sandbox-access.lua')])
endif
