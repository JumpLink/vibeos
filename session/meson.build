# The session's system files, installed by a package (debian/: vibeos-session, vibeos-daemons, sim-apn)
# instead of install.sh. Its own project, like libvibeos and the apps; the programs the units start come from the
# top-level project (input-boost, vibeos-wakeup, vibeos-auth) or from here (vibeos-dock, sim-apn).
#
# Left out on purpose: environment.d/60-spongy.conf (one phone's density) and org.gnome.Shell@.service.d/50-mobile.conf
# (a shell built into /opt by hand); install.sh, which enables the phone session, stays the hand-install path.
project('vibeos-session', version: '0.1.0', meson_version: '>= 1.0')

prefix = get_option('prefix')
sbindir = prefix / get_option('sbindir')
libexecdir = prefix / get_option('libexecdir')
datadir = get_option('datadir')
systemd_system = prefix / 'lib' / 'systemd' / 'system'
systemd_user = prefix / 'lib' / 'systemd' / 'user'

# the units name the hand-installed programs in /usr/local
local_paths = ['sed', '-e', 's|/usr/local/sbin/|' + sbindir + '/|g', '-e', 's|/usr/local/libexec/|' + libexecdir + '/|g',
               '@INPUT@']
foreach unit : ['vibeos.service', 'gnome-desktop.service', 'vibeos-dock.service', 'input-boost.service',
                'vibeos-wakeup.service', 'sim-apn.service']
  configure_file(input: unit, output: unit, command: local_paths, capture: true, install_dir: systemd_system)
endforeach

install_data('vibeos-dock', 'sim-apn', install_dir: sbindir, install_mode: 'rwxr-xr-x')
# the dock switcher's udev rule is opt-in: on a plug or an unplug vibeos-dock starts a session, which would take over a
# desktop that did not ask for it. Linked into /etc/udev/rules.d by whoever wants it
install_data('71-vibeos-dock.rules', install_dir: datadir / 'vibeos-session')
# the phone's own storage hidden from Files and the file picker (see the file)
install_data('90-vibeos-internal-storage.rules', install_dir: prefix / 'lib' / 'udev' / 'rules.d')
# the lock screen's PAM service (vibeos-auth)
install_data('pam.d/vibeos', install_dir: get_option('sysconfdir') / 'pam.d')
# vibeos-wakeup's bus name
install_data('vibeos.Wakeup.conf', install_dir: datadir / 'dbus-1' / 'system.d')
# logind leaves the power key's long press to the shell (see the file)
install_data('logind.conf.d/vibeos-power-key.conf', install_dir: prefix / 'lib' / 'systemd' / 'logind.conf.d')

# VibeOS as a session a display manager offers (gdm/): GNOME's own session with the shell in the vibeos
# mode, whose only difference is the extension, always on
install_data('gdm/vibeos.json', install_dir: datadir / 'gnome-shell' / 'modes')
install_data('gdm/vibeos.session', install_dir: datadir / 'gnome-session' / 'sessions')
install_data('gdm/vibeos.session.conf', install_dir: systemd_user / 'gnome-session@vibeos.target.d')
install_data('gdm/vibeos.desktop', install_dir: datadir / 'wayland-sessions')
# GNOME's own screen lock off: the keyguard is the lock screen (see the file)
install_data('90_vibeos.gschema.override', install_dir: datadir / 'glib-2.0' / 'schemas')
