# The names VibeOS had before (legacy-ids: gnome-phone's, up to 0.1.6 and from 0.1.7 to 0.1.11), shared with the
# launcher, and vibeos-migrate-ids, which carries a user's settings and files over to the new names once, early in
# the session (debian/: vibeos-daemons, which enables the user unit for every user).
glib = dependency('glib-2.0')
legacy_ids_dep = declare_dependency(
  link_with: static_library('legacy-ids', 'legacy-ids.c', dependencies: glib),
  include_directories: include_directories('.'),
  dependencies: glib)

# where the app packages' preinst (debian/layouts-before-vibeos.sh) saves each user's home layout before the upgrade
saved_layouts = get_option('prefix') / get_option('localstatedir') / 'lib' / 'vibeos-launcher' / 'before-vibeos'
executable('vibeos-migrate-ids', 'migrate-ids.c', dependencies: [legacy_ids_dep, dependency('gio-2.0')],
           c_args: ['-DSAVED_LAYOUTS="@0@"'.format(saved_layouts)],
           install: true, install_dir: get_option('libexecdir'))
migrate_ids_path = get_option('prefix') / get_option('libexecdir') / 'vibeos-migrate-ids'

configure_file(input: 'vibeos-migrate-ids.service.in', output: 'vibeos-migrate-ids.service',
               configuration: {'path': migrate_ids_path},
               install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')
