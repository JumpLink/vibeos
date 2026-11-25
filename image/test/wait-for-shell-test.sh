#!/bin/sh
# wait-for-shell-test.sh: the Xiaomi Pad 5's qbootctl.service as its device package changes it (nabu/), without a
# tablet. systemd-analyze verify loads qbootctl's unit (qbootctl 0.2.2-2's, below) enabled, with the drop-in, in a
# root of the test's own (the host's units beside it), and the boot's transaction: it starts after graphical.target,
# wait-for-shell first, with no ordering cycle. Then wait-for-shell in a PID
# namespace of its own (unshare: only the test's processes are seen), against a stand-in loginctl that puts the
# test's user on seat0 or on no seat, and a stand-in GNOME Shell (sleep, named gnome-shell): running steadily, absent,
# off the seat, and restarting every second.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
failed=0
check() {
	if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: $2, not $3"; failed=$((failed + 1)); fi
}

# --- the unit
R=$t/root
mkdir -p "$R/usr/lib/systemd" "$R/etc/systemd/system/qbootctl.service.d" "$R/usr/bin" "$R/usr/libexec/vibeos-device"
cp -a /usr/lib/systemd/system "$R/usr/lib/systemd/"
cat > "$R/usr/lib/systemd/system/qbootctl.service" <<'UNIT'
[Unit]
Description=Mark the Current Boot slot as Good
ConditionPathExistsGlob=/dev/disk/by-partlabel/boot_*
DefaultDependencies=no
Requires=boot-complete.target
After=local-fs.target boot-complete.target
Conflicts=shutdown.target
Before=shutdown.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/bin/qbootctl -m

[Install]
WantedBy=multi-user.target
UNIT
printf '#!/bin/sh\nexit 0\n' > "$R/usr/bin/qbootctl"; chmod 755 "$R/usr/bin/qbootctl"
install -m 644 "$here/nabu/qbootctl-wait-for-shell.conf" "$R/etc/systemd/system/qbootctl.service.d/vibeos-wait-for-shell.conf"
install -m 755 "$here/nabu/wait-for-shell" "$R/usr/libexec/vibeos-device/wait-for-shell"
systemctl --root="$R" enable qbootctl.service > /dev/null 2>&1
# graphical.target's start as the boot has it, qbootctl.service in it (multi-user.target wants it), in the debug
# log's transaction; then qbootctl.service as loaded, its drop-ins merged
SYSTEMD_LOG_LEVEL=debug systemd-analyze verify --root="$R" graphical.target > "$t/verify.txt" 2>&1 || true
SYSTEMD_LOG_LEVEL=debug systemd-analyze verify --root="$R" qbootctl.service > "$t/dump.txt" 2>&1 || true
awk '/→ Unit qbootctl.service:/ {p = 1; next} p && /→ Unit / {p = 0} p' "$t/dump.txt" > "$t/unit.txt"
check "no ordering cycle in the boot's transaction" "$(grep -c -i 'ordering cycle' "$t/verify.txt" || true)" 0
check "qbootctl.service is in it" "$(grep -c 'Installed new job qbootctl.service/start' "$t/verify.txt" || true)" 1
check "the drop-in is read with the unit" "$(grep -c 'DropIn Path: .*/qbootctl.service.d/vibeos-wait-for-shell.conf$' "$t/unit.txt" || true)" 1
check "it starts after graphical.target" "$(grep -c -E '^[[:space:]]+After: graphical.target ' "$t/unit.txt" || true)" 1
check "wait-for-shell first, then qbootctl -m" "$(sed -n 's/^[[:space:]]*Command Line: //p' "$t/unit.txt" | tr '\n' ' ')" \
	"/usr/libexec/vibeos-device/wait-for-shell 120 /usr/bin/qbootctl -m "
check "a start timeout past wait-for-shell's limit" "$(sed -n 's/^[[:space:]]*TimeoutStartSec: //p' "$t/unit.txt")" 3min
systemd-analyze verify --root="$R" qbootctl.service > "$t/warn.txt" 2>&1 || true
check "systemd-analyze verify: no warning about qbootctl.service" \
	"$(grep -i -E 'qbootctl|wait-for-shell' "$t/warn.txt" | head -n 3 | tr '\n' ' ')" ""

# --- wait-for-shell: a stand-in loginctl (the test's user, uid 0 in the namespace, has session c1 on $t/seat)
mkdir -p "$t/bin"
cat > "$t/bin/loginctl" <<'FAKE'
#!/bin/sh
case "$1 $2" in
"show-user 0") echo c1 ;;
"show-session c1") cat "$WFS_SEAT" ;;
*) exit 1 ;;
esac
FAKE
chmod 755 "$t/bin/loginctl"
cp "$(command -v sleep)" "$t/bin/gnome-shell"
# run NAME SETUP LIMIT STEADY: wait-for-shell's exit status with SETUP's processes started first
run() {
	WFS_SEAT=$t/seat PATH=$t/bin:$PATH unshare -Urpf --mount-proc sh -c "$1
		sh '$here/nabu/wait-for-shell' $2 $3 > /dev/null 2>&1 && echo 0 || echo 1"
}
echo seat0 > "$t/seat"
check "a shell on seat0, steady: good" "$(run 'gnome-shell 30 & sleep 1;' 8 2)" 0
check "no shell: unmarked after the limit" "$(run ':' 4 2)" 1
echo > "$t/seat"
check "a shell whose user is on no seat: unmarked" "$(run 'gnome-shell 30 & sleep 1;' 4 2)" 1
echo seat0 > "$t/seat"
check "a shell that restarts every second: unmarked" \
	"$(run '(while :; do gnome-shell 1; done) & ' 8 3)" 1
check "a shell that has run for a while already: good at once" "$(run 'gnome-shell 30 & sleep 3;' 0 2)" 0

[ "$failed" = 0 ] && echo "all passed" || { echo "$failed failed"; exit 1; }
