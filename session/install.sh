#!/bin/sh
# Installs the session files on the phone (as root): the session unit, the shell's drop-in, the old launcher's
# condition, the input boost (its binary from the build: /usr/local/sbin/input-boost), the lock screen's PAM
# service (for /usr/local/libexec/gp-auth), the session's environment (libspongy's density), the dock switcher and
# sim-apn (a mobile data profile for every new SIM).
# GNOME Phone becomes the session the phone boots into; Phosh stays installed as its fallback (OnFailure).
set -e
cd "$(dirname "$0")"
install -m 644 gnome-phone.service input-boost.service sim-apn.service /etc/systemd/system/
install -m 755 neo-dock sim-apn /usr/local/sbin/
install -m 644 pam.d/gnome-phone /etc/pam.d/
install -m 644 environment.d/60-spongy.conf /etc/environment.d/
install -d /etc/systemd/user/org.gnome.Shell@.service.d /etc/systemd/user/neo-launcher.service.d
install -m 644 org.gnome.Shell@.service.d/50-mobile.conf /etc/systemd/user/org.gnome.Shell@.service.d/
install -m 644 neo-launcher.service.d/80-phosh-only.conf /etc/systemd/user/neo-launcher.service.d/
systemctl daemon-reload
systemctl enable input-boost.service gnome-phone.service sim-apn.service
systemctl disable phosh.service
