#!/bin/sh
set -eu

build_dir=${1:-build}
socket_path=/tmp/openrsp-api-hardware-gate.sock
daemon_log=/tmp/openrsp-api-hardware-gate-daemon.log
client_log=/tmp/openrsp-api-hardware-gate-client.log
daemon_pid=

cleanup()
{
    if [ -n "$daemon_pid" ]; then
        kill -TERM "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -f "$socket_path"
}
trap cleanup EXIT INT TERM

if ! ioreg -p IOUSB -l -w 0 | grep -q 'idProduct" = 12320'; then
    echo RSPDUO_NOT_ENUMERATED >&2
    exit 2
fi

rm -f "$socket_path" "$daemon_log" "$client_log"
OPENRSPD_SOCKET="$socket_path" "$build_dir/openrspd" >"$daemon_log" 2>&1 &
daemon_pid=$!

attempt=0
while [ ! -S "$socket_path" ] && [ "$attempt" -lt 50 ]; do
    sleep 0.1
    attempt=$((attempt + 1))
done
if [ ! -S "$socket_path" ]; then
    echo OPENRSPD_DID_NOT_START >&2
    cat "$daemon_log" >&2
    exit 3
fi

timeout_command=
if command -v gtimeout >/dev/null 2>&1; then
    timeout_command=gtimeout
elif command -v timeout >/dev/null 2>&1; then
    timeout_command=timeout
fi

set +e
if [ -n "$timeout_command" ]; then
    OPENRSPD_SOCKET="$socket_path" "$timeout_command" -k 2 12 \
        "$build_dir/sdrplay-compat-stream-test" \
        853900000 851900000 4 2048000 0 1 >"$client_log" 2>&1
else
    OPENRSPD_SOCKET="$socket_path" "$build_dir/sdrplay-compat-stream-test" \
        853900000 851900000 4 2048000 0 1 >"$client_log" 2>&1
fi
result=$?
set -e

printf 'client_rc=%s\n' "$result"
cat "$client_log"
printf '%s\n' '--- daemon ---'
cat "$daemon_log"
exit "$result"
