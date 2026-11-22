# vibeos-unifiedpush: the UnifiedPush distributor (the D-Bus spec 0.3.0), on the VibeOS push server (vup-main.c)
up_deps = [dependency('gio-unix-2.0', version: '>= 2.76'), dependency('libsoup-3.0'), dependency('json-glib-1.0')]
up_resources = import('gnome').compile_resources('vup-resources', 'data/vibeos-unifiedpush.gresource.xml',
                                                  source_dir: 'data', c_name: 'vup')
up_args = ['-DG_LOG_DOMAIN="vibeos-unifiedpush"']
up_common = static_library('vup-common', 'vup-util.c', c_args: up_args, dependencies: up_deps)
executable('vibeos-unifiedpush', 'vup-main.c', 'vup-store.c', 'vup-stream.c', 'vup-deliver.c', up_resources,
           link_with: up_common, dependencies: up_deps, c_args: up_args + ['-DVERSION="@0@"'.format(meson.project_version())],
           install: true, install_dir: get_option('libexecdir'))

up_path = {'path': get_option('prefix') / get_option('libexecdir') / 'vibeos-unifiedpush'}
configure_file(input: 'data/vibeos-unifiedpush.service.in', output: 'vibeos-unifiedpush.service',
               configuration: up_path, install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')
configure_file(input: 'data/org.unifiedpush.Distributor.vibeos.service.in',
               output: 'org.unifiedpush.Distributor.vibeos.service',
               configuration: up_path, install_dir: get_option('datadir') / 'dbus-1' / 'services')
install_data('data/vibeos.UnifiedPush.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
install_data('data/vibeos.UnifiedPush1.xml', install_dir: get_option('datadir') / 'dbus-1' / 'interfaces')

# topics, the redaction of secrets, the checks of what apps send
test('unifiedpush-util', executable('test-vup-util', 'tests/test-vup-util.c', link_with: up_common,
                                    dependencies: up_deps, include_directories: include_directories('.')))
