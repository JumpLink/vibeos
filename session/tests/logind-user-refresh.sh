#!/bin/sh
# vibeos-logind-user-refresh against a stand-in loginctl and systemd-run: nothing is done while logind's record agrees
# with logind, the record is written again when it does not, and with no active session on seat0 it gives up.
# $1: the session project's source directory
set -u
src=$1
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
mkdir -p "$t/bin" "$t/users"
cat > "$t/bin/loginctl" <<'MOCK'
#!/bin/sh
# show-seat seat0 / show-session ID / show-user UID, -p NAME --value: answers from $T's files
case "$1 $4" in
"show-seat ActiveSession") cat "$T/active" 2>/dev/null ;;
"show-session Active") echo yes ;;
"show-session User") echo 1000 ;;
"show-session Name") echo owner ;;
"show-user State") echo active ;;
esac
MOCK
cat > "$t/bin/systemd-run" <<'MOCK'
#!/bin/sh
echo "$*" >> "$T/ran"
printf 'STATE=active\nACTIVE_SESSIONS=2\n' > "$T/users/1000"
MOCK
chmod +x "$t/bin/loginctl" "$t/bin/systemd-run"
run() {
	rm -f "$t/ran"
	env T="$t" PATH="$t/bin:$PATH" VIBEOS_LOGIND_USERS="$t/users" VIBEOS_LOGIND_TRIES=2 sh "$src/vibeos-logind-user-refresh"
}
fail=0
check() {
	if [ "$1" = ok ]; then echo "ok - $2"; else echo "not ok - $2"; fail=1; fi
}

echo 2 > "$t/active"
printf 'STATE=active\nACTIVE_SESSIONS=2\n' > "$t/users/1000"
out=$(run)
[ ! -e "$t/ran" ] && echo "$out" | grep -q "record is current" && r=ok || r=bad
check $r "the record agrees with logind: nothing done ($out)"

printf 'STATE=online\nACTIVE_SESSIONS=\n' > "$t/users/1000"
out=$(run)
[ -e "$t/ran" ] && grep -q -- "--uid=1000 -p PAMName=login --wait --collect /bin/true" "$t/ran" \
	&& echo "$out" | grep -q "now STATE=active, ACTIVE_SESSIONS=2" && r=ok || r=bad
check $r "STATE=online: a PAM session writes the record again"

printf 'STATE=active\nACTIVE_SESSIONS=3\n' > "$t/users/1000"
out=$(run)
[ -e "$t/ran" ] && r=ok || r=bad
check $r "active, but another session listed: written again"

rm -f "$t/active"
out=$(run)
[ ! -e "$t/ran" ] && echo "$out" | grep -q "no active session on seat0" && r=ok || r=bad
check $r "no active session on seat0: gives up, nothing done"
exit $fail
