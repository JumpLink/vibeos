#!/bin/sh
# pam-vibeos.sh SOURCE_DIR: the lock screen's PAM service unlocks the login keyring. With auto-login no password
# reaches pam_gnome_keyring at boot, so the keyring stays locked until the PIN is checked: the service must
# run the module in its auth stack, after common-auth (which is what sets PAM_AUTHTOK), as optional.
set -eu
f=$1/session/pam.d/vibeos
rules=$(grep -v '^[[:space:]]*#' "$f" | grep -v '^[[:space:]]*$')
auth=$(printf '%s\n' "$rules" | grep -n '^@include common-auth$' | cut -d: -f1)
key=$(printf '%s\n' "$rules" | grep -n '^auth[[:space:]]\+optional[[:space:]]\+pam_gnome_keyring.so\([[:space:]]\|$\)' | cut -d: -f1)
[ -n "$auth" ] || { echo "FAIL: no @include common-auth in $f" >&2; exit 1; }
[ -n "$key" ] || { echo "FAIL: no 'auth optional pam_gnome_keyring.so' in $f" >&2; exit 1; }
[ "$key" -gt "$auth" ] || { echo "FAIL: pam_gnome_keyring runs before common-auth set the password" >&2; exit 1; }
echo "ok: pam_gnome_keyring follows common-auth in $f"
