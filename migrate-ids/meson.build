# The IDs before 0.1.7 and their new names (legacy-ids), shared with the launcher, and gnome-phone-migrate-ids,
# which carries a user's settings and files over to the new IDs once, early in the session (debian/:
# gnome-phone-daemons, which enables the user unit for every user).
glib = dependency('glib-2.0')
legacy_ids_dep = declare_dependency(
  link_with: static_library('legacy-ids', 'legacy-ids.c', dependencies: glib),
  include_directories: include_directories('.'),
  dependencies: glib)

executable('gnome-phone-migrate-ids', 'migrate-ids.c', dependencies: [legacy_ids_dep, dependency('gio-2.0')],
           install: true, install_dir: get_option('libexecdir'))
migrate_ids_path = get_option('prefix') / get_option('libexecdir') / 'gnome-phone-migrate-ids'

configure_file(input: 'gnome-phone-migrate-ids.service.in', output: 'gnome-phone-migrate-ids.service',
               configuration: {'path': migrate_ids_path},
               install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')
