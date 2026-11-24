#!/bin/sh
# uim-provision-test.sh: files/qcom-uim-provision against a stand-in qmicli that answers --uim-get-card-status with
# what the OnePlus 6T's modem reports (the layout of libqmi 1.38's output; the AIDs made up) and records every other
# call: the session opened on the USIM's slot and AID when none exists, nothing done when one is open, when no card
# is present or when the card has no USIM. Needs only sh and awk; touches only a temporary directory.
set -u
T=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FAILS=0
mkdir "$W/bin"
cat > "$W/bin/qmicli" <<'EOF'
#!/bin/sh
case "$3" in
--uim-get-card-status) cat "$CARD" ;;
*) echo "$*" >> "$CALLS"; echo "[qrtr://0] Successfully changed provisioning session" ;;
esac
EOF
chmod 755 "$W/bin/qmicli"

# card SESSION SLOT1-STATE SLOT2-STATE APP2-TYPE: a card status with two slots
card() {
	cat <<EOF
[qrtr://0] Successfully got card status
Provisioning applications:
	Primary GW:   $1
	Primary 1X:   session doesn't exist
	Secondary GW: session doesn't exist
	Secondary 1X: session doesn't exist
Slot [1]:
	Card state: '$2'
	UPIN state: 'not-initialized'
		UPIN retries: '0'
		UPUK retries: '0'
EOF
	[ "$2" = present ] && cat <<EOF
	Application [1]:
		Application type:  'isim (5)'
		Application state: 'detected'
		Application ID:
			A0:00:00:00:87:10:04:FF:FF:FF:FF:89:00:00:00:01
		Personalization state: 'unknown'
EOF
	cat <<EOF
Slot [2]:
	Card state: '$3'
EOF
	[ "$3" = present ] && cat <<EOF
	Application [1]:
		Application type:  '$4'
		Application state: 'detected'
		Application ID:
			A0:00:00:00:87:10:02:FF:FF:FF:FF:89:00:00:00:02
		Personalization state: 'unknown'
		PIN1 state: 'not-initialized'
EOF
}

# run NAME WANT-OUTPUT WANT-CALL: the script on $W/card, its last line and the call it made ("" for none)
run() {
	: > "$W/calls"
	out=$(PATH="$W/bin:$PATH" CARD="$W/card" CALLS="$W/calls" UIM_TRIES=2 UIM_SLEEP=0 sh "$T/files/qcom-uim-provision" | tail -1)
	call=$(cat "$W/calls")
	if [ "$out" = "$2" ] && [ "$call" = "$3" ]; then echo "  ok: $1"
	else echo "  FAIL: $1: said '$out', called '$call'"; FAILS=$((FAILS + 1)); fi
}

card "session doesn't exist" "error: no-atr-received (3)" present "usim (2)" > "$W/card"
run "closed session: opened on the USIM's slot and AID" "primary GW session opened on slot 2" \
	"-d qrtr://0 --uim-change-provisioning-session=slot=2,activate=yes,session-type=primary-gw-provisioning,aid=A0:00:00:00:87:10:02:FF:FF:FF:FF:89:00:00:00:02"
card "slot '2', application '1'" "error: no-atr-received (3)" present "usim (2)" > "$W/card"
run "session open: nothing done" "primary GW session already open" ""
card "session doesn't exist" "error: no-atr-received (3)" absent "" > "$W/card"
run "no card: nothing done" "no card present" ""
card "session doesn't exist" "error: no-atr-received (3)" present "csim (4)" > "$W/card"
run "no USIM: nothing done" "no USIM application" ""
card "session doesn't exist" present absent "" > "$W/card"
run "only an ISIM in the present slot: nothing done" "no USIM application" ""

[ $FAILS = 0 ] && echo "all ok" || { echo "$FAILS failed"; exit 1; }
