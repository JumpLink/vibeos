#!/bin/sh
# Installs the session files on the phone (as root): the session unit, the shell's drop-in, the old launcher's
# condition, the input boost (its binary from the build: /usr/local/sbin/input-boost) and the lock screen's PAM
# service (for /usr/local/libexec/gp-auth)
set -e
cd "$(dirname "$0")"
install -m 644 gnome-phone.service input-boost.service /etc/systemd/system/
install -m 644 pam.d/gnome-phone /etc/pam.d/
install -d /etc/systemd/user/org.gnome.Shell@.service.d /etc/systemd/user/neo-launcher.service.d
install -m 644 org.gnome.Shell@.service.d/50-mobile.conf /etc/systemd/user/org.gnome.Shell@.service.d/
install -m 644 neo-launcher.service.d/80-phosh-only.conf /etc/systemd/user/neo-launcher.service.d/
systemctl daemon-reload
systemctl enable input-boost.service
