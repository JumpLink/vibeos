#!/bin/sh
# voltd-wait-test.sh: the wait of files/81voltd-wait-modem.conf, run as systemd would run it ($$ as $), against a
# stand-in mmcli that lists no modem for its first calls: it returns once the modem is listed, and when none ever
# is it gives up and still lets 81voltd start. Needs only sh, sed and grep; touches only a temporary directory.
set -u
T=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FAILS=0
mkdir "$W/bin"
cat > "$W/bin/mmcli" <<'EOS'
#!/bin/sh
echo . >> "$CALLS"
if [ "$(wc -l < "$CALLS")" -gt "$EMPTY" ]; then
	echo "    /org/freedesktop/ModemManager1/Modem/0 [QUALCOMM INCORPORATED] 0"
else
	echo "No modems were found"
fi
EOS
printf '#!/bin/sh\n' > "$W/bin/sleep"
chmod 755 "$W/bin/mmcli" "$W/bin/sleep"
cmd=$(sed -n "s/^ExecStartPre=\/bin\/sh -c '\(.*\)'$/\1/p" "$T/files/81voltd-wait-modem.conf" | sed 's/\$\$/$/g')
[ -n "$cmd" ] || { echo "FAIL: no ExecStartPre in the drop-in"; exit 1; }

# run NAME EMPTY WANT-CALLS WANT-STATUS: the wait with a modem listed from call EMPTY+1 on
run() {
	: > "$W/calls"
	PATH="$W/bin:$PATH" CALLS="$W/calls" EMPTY="$2" sh -c "$cmd" > /dev/null; st=$?
	n=$(wc -l < "$W/calls" | tr -d ' ')
	if [ "$n" = "$3" ] && [ "$st" = "$4" ]; then echo "  ok: $1"
	else echo "  FAIL: $1: $n calls, exit $st"; FAILS=$((FAILS + 1)); fi
}
run "modem there at once" 0 1 0
run "modem after 5 s" 5 6 0
run "no modem: gives up after 90 tries, 81voltd starts anyway" 1000 90 0

[ $FAILS = 0 ] && echo "all ok" || { echo "$FAILS failed"; exit 1; }
