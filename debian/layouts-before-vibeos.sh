#!/bin/sh
# preinst of every app package (linked as <package>.preinst). Before VibeOS the apps were gnome-phone's, under other
# package names and desktop ids. The old launcher (neo-launcher, gnome-phone up to 0.1.11) still running in a session
# while the packages are swapped sees each app's desktop file go and the renamed one come, an app it has never seen:
# it empties the old one's cells, puts the new one on the first free cell and saves that over the user's home layout.
# So before the first renamed app is unpacked, each user's layout is saved to
# /var/lib/vibeos-launcher/before-vibeos/<user>.json, readable by that user only; vibeos-migrate-ids puts it back at
# the next login, before the new launcher reads it (and the new launcher reads the old ids in it as the new ones).
set -e

SAVED=/var/lib/vibeos-launcher/before-vibeos
# the launcher's directory and layout file before VibeOS. Once vibeos-migrate-ids has moved a user over, the
# directory is a symlink to the new one: nothing to save for that user then.
OLD_DIR=.config/neo-launcher

save_layouts() {
    # once, by the first of the app packages: by the second the first one's desktop file is gone
    [ ! -e "$SAVED/done" ] || return 0
    mkdir -p "$SAVED"
    getent passwd | while IFS=: read -r name _ uid gid _ dir _; do
        [ "$uid" -ge 1000 ] 2>/dev/null && [ "$uid" -lt 60000 ] || continue
        [ -d "$dir/$OLD_DIR" ] && [ ! -L "$dir/$OLD_DIR" ] || continue
        # read as the user: root would follow wherever the user's file points
        if (umask 077; timeout 10 setpriv --reuid="$uid" --regid="$gid" --init-groups -- \
                sh -c '[ -f "$1" ] && exec cat -- "$1"' sh "$dir/$OLD_DIR/layout.json" \
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
    # whichever renamed package comes first (the launcher's package may already have replaced the old launcher's
    # files; the old launcher itself still runs until the next login)
    save_layouts || echo "could not save the home layouts before the upgrade" >&2
    ;;
esac

#DEBHELPER#

exit 0
