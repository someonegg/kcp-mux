#!/bin/sh
set -eu

SERVER=$1
CLIENT=$2
PORT=$((20000 + ($$ % 20000)))
TMPDIR=${TMPDIR:-/tmp}
LOGDIR=$(mktemp -d "$TMPDIR/kcpmux-demo.XXXXXX")
SERVER_LOG="$LOGDIR/server.log"
CLIENT_LOG="$LOGDIR/client.log"

cleanup() {
    if [ "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$LOGDIR"
}
trap cleanup EXIT INT TERM

"$SERVER" --host 127.0.0.1 --port "$PORT" --quiet >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

sleep 0.2

if ! "$CLIENT" --host 127.0.0.1 --port "$PORT" --streams 3 --message smoke --timeout-ms 5000 \
    >"$CLIENT_LOG" 2>&1
then
    echo "demo client failed" >&2
    echo "--- server log ---" >&2
    cat "$SERVER_LOG" >&2 || true
    echo "--- client log ---" >&2
    cat "$CLIENT_LOG" >&2 || true
    exit 1
fi

if ! grep "OK streams=3" "$CLIENT_LOG" >/dev/null 2>&1; then
    echo "demo client did not report success" >&2
    cat "$CLIENT_LOG" >&2 || true
    exit 1
fi

cat "$CLIENT_LOG"
