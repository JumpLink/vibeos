#!/bin/sh
# files-servers.sh: throwaway WebDAV, FTP and SMB servers for the files rig (try-files.sh). They run
# on the development machine, on 127.0.0.1 only, and reach the phone's 127.0.0.1 through `ssh -R`, so the phone needs
# no server packages and nothing listens on the LAN. Each serves DIR/share, a test tree of its own, to one throwaway
# account (rig / rig-secret); its password is what the app saves in the rig's keyring.
#
#   files-servers.sh setup DIR    a venv in DIR with pyftpdlib, wsgidav, cheroot and impacket, and the test tree
#   files-servers.sh start DIR    the servers, and the tunnels to PHONE (root@host; SSH_OPTS for ssh's options)
#   files-servers.sh stop DIR     the servers and the tunnels
#
# On the phone:  dav://127.0.0.1:8088/  ftp://127.0.0.1:2121/ (passive 2130-2139)  smb://127.0.0.1:4455/share
set -e

DIR=$2
USER_NAME=rig
PASSWORD=rig-secret
DAV=8088
FTP=2121
PASV_FIRST=2130
PASV_LAST=2139
SMB=4455

if [ -z "$DIR" ]; then
    awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
    exit 2
fi

case $1 in
setup)
    mkdir -p "$DIR/share/inside" "$DIR/run"
    [ -x "$DIR/venv/bin/python" ] || python3 -m venv "$DIR/venv"
    "$DIR/venv/bin/pip" install -q pyftpdlib wsgidav cheroot impacket
    printf '%s\n' "on the rig's own server" >"$DIR/share/hello.txt"
    head -c 2000000 /dev/urandom >"$DIR/share/two-megabytes.bin"
    printf '%s\n' "a level down" >"$DIR/share/inside/deeper.txt"
    cat >"$DIR/run/wsgidav.yaml" <<EOF
host: 127.0.0.1
port: $DAV
provider_mapping:
    "/": "$DIR/share"
http_authenticator:
    accept_basic: true
    accept_digest: true
    default_to_digest: true
simple_dc:
    user_mapping:
        "*":
            "$USER_NAME":
                password: "$PASSWORD"
verbose: 1
EOF
    ;;
start)
    : "${PHONE:?PHONE=root@phone}"
    cd "$DIR/run"
    "$DIR/venv/bin/wsgidav" --config "$DIR/run/wsgidav.yaml" >dav.log 2>&1 &
    echo $! >dav.pid
    "$DIR/venv/bin/python" -m pyftpdlib -i 127.0.0.1 -p $FTP -r $PASV_FIRST-$PASV_LAST -w \
        -u $USER_NAME -P $PASSWORD -d "$DIR/share" >ftp.log 2>&1 &
    echo $! >ftp.pid
    "$DIR/venv/bin/python" "$DIR/venv/bin/smbserver.py" -smb2support -ip 127.0.0.1 -port $SMB \
        -username $USER_NAME -password $PASSWORD share "$DIR/share" >smb.log 2>&1 &
    echo $! >smb.pid
    # the phone's 127.0.0.1:PORT to ours, the FTP data ports too (the server tells the client 127.0.0.1)
    forwards="-R 127.0.0.1:$DAV:127.0.0.1:$DAV -R 127.0.0.1:$FTP:127.0.0.1:$FTP -R 127.0.0.1:$SMB:127.0.0.1:$SMB"
    port=$PASV_FIRST
    while [ $port -le $PASV_LAST ]; do
        forwards="$forwards -R 127.0.0.1:$port:127.0.0.1:$port"
        port=$((port + 1))
    done
    # shellcheck disable=SC2086
    ssh $SSH_OPTS -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 $forwards "$PHONE" >tunnel.log 2>&1 &
    echo $! >tunnel.pid
    sleep 2
    for p in dav ftp smb tunnel; do
        kill -0 "$(cat $p.pid)" 2>/dev/null || { echo "files-servers: $p did not start, see $DIR/run/$p.log" >&2; exit 1; }
    done
    ;;
stop)
    for p in tunnel smb ftp dav; do
        [ -f "$DIR/run/$p.pid" ] && kill "$(cat "$DIR/run/$p.pid")" 2>/dev/null || true
        rm -f "$DIR/run/$p.pid"
    done
    ;;
*)
    awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
    exit 2
    ;;
esac
