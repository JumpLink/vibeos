#!/bin/sh
# vibeos-hide-replaced-apps on a made-up /usr/share/applications: GNOME's entry hidden only while the VibeOS app is
# there, its other keys and actions kept, made again when GNOME's file changes, gone with either app, and a file of
# the same name that the script did not write never touched. $1: the session's source directory
set -u
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export VIBEOS_APPS_DIR=$T/apps VIBEOS_HIDDEN_DIR=$T/hidden
mkdir -p "$T/apps"
run() { d=$1; shift; sh "$d/vibeos-hide-replaced-apps" "$@" 2> "$T/err"; }
S=$1; shift
fail=0
ok() { echo "ok - $1"; }
no() { echo "not ok - $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else no "$1"; fi; }

cat > "$T/apps/org.gnome.Nautilus.desktop" <<'X'
[Desktop Entry]
Name=Files
Exec=nautilus --new-window %U
NoDisplay=false
MimeType=inode/directory;
DBusActivatable=true

[Desktop Action new-window]
Name=New Window
NoDisplay=false
Exec=nautilus --new-window
X
cat > "$T/apps/org.gnome.Console.desktop" <<'X'
[Desktop Entry]
Name=Console
Exec=kgx
X
H=$T/hidden/org.gnome.Nautilus.desktop

run "$S"
check "nothing hidden without the VibeOS apps" "[ ! -e '$T/hidden' ] || [ -z \"\$(ls '$T/hidden')\" ]"

touch "$T/apps/vibeos.Files.desktop"
run "$S"
check "Nautilus hidden once Files is there" "[ -f '$H' ]"
check "Console not hidden without Terminal" "[ ! -e '$T/hidden/org.gnome.Console.desktop' ]"
check "one NoDisplay in the entry, and it is true" \
	"[ \"\$(awk '/^\\[/ {e = (\$0 == \"[Desktop Entry]\")} e && /^NoDisplay=/' '$H')\" = NoDisplay=true ]"
check "the other keys kept" "grep -qx 'MimeType=inode/directory;' '$H' && grep -qx 'Exec=nautilus --new-window %U' '$H'"
check "the action's own keys kept" "grep -c '^NoDisplay=false' '$H' | grep -qx 1 && grep -qx '\\[Desktop Action new-window\\]' '$H'"
check "the copy is readable by all" "[ \"\$(stat -c %a '$H')\" = 644 ]"
check "its file types in the directory's cache" \
	"grep -qx 'inode/directory=org.gnome.Nautilus.desktop;' '$T/hidden/mimeinfo.cache' && grep -qx '\\[MIME Cache\\]' '$T/hidden/mimeinfo.cache'"

before=$(stat -c %Y.%i "$H")
sleep 1; run "$S"
check "unchanged when GNOME's file is unchanged" "[ \"\$(stat -c %Y.%i '$H')\" = '$before' ]"

sed -i 's/^Name=Files$/Name=Files 2/' "$T/apps/org.gnome.Nautilus.desktop"
run "$S"
check "made again when GNOME's file changes" "grep -qx 'Name=Files 2' '$H'"

rm "$T/apps/vibeos.Files.desktop"; run "$S"
check "shown again when Files is removed" "[ ! -e '$H' ]"
check "and the cache with it" "[ ! -e '$T/hidden/mimeinfo.cache' ]"

touch "$T/apps/vibeos.Files.desktop"; run "$S"
rm "$T/apps/org.gnome.Nautilus.desktop"; run "$S"
check "the copy goes with Nautilus" "[ ! -e '$H' ]"

touch "$T/apps/vibeos.Terminal.desktop"
printf '[Desktop Entry]\nName=Mine\n' > "$T/hidden/org.gnome.Console.desktop"
run "$S"
check "a file the script did not write is left as it is" "grep -qx 'Name=Mine' '$T/hidden/org.gnome.Console.desktop'"
check "and that is said" "grep -q 'not ours' '$T/err'"
rm "$T/hidden/org.gnome.Console.desktop"; run "$S"
check "Console hidden once Terminal is there" "[ -f '$T/hidden/org.gnome.Console.desktop' ]"

printf '[Desktop Entry]\nName=Other\nMimeType=text/plain;\n' > "$T/hidden/other.desktop"
run "$S"
check "another file's types are not in the cache" "[ ! -e '$T/hidden/mimeinfo.cache' ] || ! grep -q text/plain '$T/hidden/mimeinfo.cache'"
run "$S" --clear
check "--clear removes the copies" "[ ! -e '$T/hidden/org.gnome.Console.desktop' ] && [ ! -e '$T/hidden/mimeinfo.cache' ]"
check "--clear leaves other files" "[ -f '$T/hidden/other.desktop' ]"
printf '[MIME Cache]\n' > "$T/hidden/mimeinfo.cache"
run "$S"
check "a cache the script did not write is left as it is" "[ \"\$(cat '$T/hidden/mimeinfo.cache')\" = '[MIME Cache]' ]"

exit $fail
