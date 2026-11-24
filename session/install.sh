#!/bin/sh
# Installs the session files on the phone (as root): the session unit, the shell's drop-in, the input boost (its
# binary from the build: /usr/local/sbin/input-boost), the lock screen's PAM service (for /usr/local/libexec/vibeos-auth),
# the session's environment (libspongy's density) and the dock switcher (vibeos-dock, its unit and udev rule, and the
# docked desktop's session unit), sim-apn (a mobile data profile for every new SIM) and vibeos-wakeup (wakes the phone
# for alarm clocks; its binary from the build: /usr/local/sbin/vibeos-wakeup), and logind's drop-in that leaves the power
# key's long press to the shell (read at the next boot).
# VibeOS becomes the session the phone boots into.
set -e
cd "$(dirname "$0")"
install -m 644 vibeos.service gnome-desktop.service vibeos-dock.service input-boost.service sim-apn.service \
    vibeos-wakeup.service /etc/systemd/system/
install -m 755 vibeos-dock sim-apn /usr/local/sbin/
install -m 644 71-vibeos-dock.rules /etc/udev/rules.d/
install -m 644 90-vibeos-internal-storage.rules /etc/udev/rules.d/
install -m 644 pam.d/vibeos /etc/pam.d/
install -m 644 environment.d/60-spongy.conf /etc/environment.d/
install -m 644 vibeos.Wakeup.conf /etc/dbus-1/system.d/
install -D -m 644 logind.conf.d/vibeos-power-key.conf /etc/systemd/logind.conf.d/vibeos-power-key.conf
install -d /etc/systemd/user/org.gnome.Shell@.service.d
install -m 644 org.gnome.Shell@.service.d/50-mobile.conf /etc/systemd/user/org.gnome.Shell@.service.d/
systemctl reload dbus
systemctl daemon-reload
systemctl enable input-boost.service vibeos.service vibeos-dock.service sim-apn.service vibeos-wakeup.service
