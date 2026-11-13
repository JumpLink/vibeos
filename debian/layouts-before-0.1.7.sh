#!/bin/sh
# preinst of every app package (linked as <package>.preinst). Up to 0.1.6 the apps had other desktop ids. A 0.1.6
# neo-launcher still running in a session while the packages are upgraded sees each app's desktop file go and the
# renamed one come, an app it has never seen: it empties the old one's cells, puts the new one on the first free cell
# and saves that over the user's home layout. So before the first renamed app is unpacked, each user's layout is
# saved to /var/lib/neo-launcher/before-0.1.7/<user>.json, readable by that user only; gnome-phone-migrate-ids puts it
# back at the next login, before the new launcher reads it (and the new launcher reads the old ids in it as the new
# ones).
set -e

SAVED=/var/lib/neo-launcher/before-0.1.7

save_layouts() {
    # once, by the first of the app packages: by the second the first one's desktop file is gone
    [ ! -e "$SAVED/done" ] || return 0
    mkdir -p "$SAVED"
    getent passwd | while IFS=: read -r name _ uid gid _ dir _; do
        [ "$uid" -ge 1000 ] 2>/dev/null && [ "$uid" -lt 60000 ] || continue
        # read as the user: root would follow wherever the user's file points
        if (umask 077; timeout 10 setpriv --reuid="$uid" --regid="$gid" --init-groups -- \
                sh -c '[ -f "$1" ] && exec cat -- "$1"' sh "$dir/.config/neo-launcher/layout.json" \
                > "$SAVED/$name.json.new") 2>/dev/null; then
            chown "$uid:$gid" "$SAVED/$name.json.new"
            mv "$SAVED/$name.json.new" "$SAVED/$name.json"
        else
            rm -f "$SAVED/$name.json.new"
        fi
    done
    touch "$SAVED/done"
}

case "$1" in
install|upgrade)
    # $2: the version installed before (for install, one removed but not purged)
    if [ -n "$2" ] && dpkg --compare-versions "$2" lt 0.1.7~; then
        save_layouts || echo "could not save the home layouts before the upgrade" >&2
    fi
    ;;
esac

#DEBHELPER#

exit 0
