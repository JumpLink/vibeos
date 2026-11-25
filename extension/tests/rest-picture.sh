#!/bin/sh
# rest-picture.sh SRC: restpicture.js (the lock screen's picture of the wallpaper at rest, made by the launcher and kept
# in the cache, and the order the lock screen shows it in) against a stand-in launcher and stand-ins for the keyguard's
# decoding and showing, driven by rest-picture.js; a cache of the test's own. Only Gio and GLib are used; no shell, no
# launcher, no display. About 5 s: the timeouts it checks are shortened to 0.5-1 s.
set -eu
src=$1
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
cp "$src/restpicture.js" "$t/restpicture.js"
cp "$here/rest-picture.js" "$t/test.js"
XDG_CACHE_HOME=$t/cache gjs -m "$t/test.js" "$t"
