#!/bin/sh

set -eu

binary=${1:-./fastcompmgr}

if ! command -v Xvfb >/dev/null 2>&1; then
    echo "Xvfb smoke test: skipped (Xvfb is not installed)"
    exit 0
fi

if [ ! -x "$binary" ]; then
    echo "Xvfb smoke test: compositor is not executable: $binary" >&2
    exit 1
fi

smoke_dir=$(mktemp -d)
display_file=$smoke_dir/display
xvfb_log=$smoke_dir/xvfb.log
compositor_log=$smoke_dir/fastcompmgr.log
xvfb_pid=
compositor_pid=
client_pid=

cleanup()
{
    if [ -n "$client_pid" ]; then
        kill "$client_pid" 2>/dev/null || :
        wait "$client_pid" 2>/dev/null || :
    fi
    if [ -n "$compositor_pid" ]; then
        kill "$compositor_pid" 2>/dev/null || :
        wait "$compositor_pid" 2>/dev/null || :
    fi
    if [ -n "$xvfb_pid" ]; then
        kill "$xvfb_pid" 2>/dev/null || :
        wait "$xvfb_pid" 2>/dev/null || :
    fi
    rm -rf "$smoke_dir"
}
trap cleanup EXIT HUP INT TERM

Xvfb -displayfd 3 -screen 0 640x480x24 -nolisten tcp \
    3>"$display_file" >"$xvfb_log" 2>&1 &
xvfb_pid=$!

attempt=0
while [ ! -s "$display_file" ] && kill -0 "$xvfb_pid" 2>/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo "Xvfb smoke test: Xvfb did not become ready" >&2
        sed -n '1,120p' "$xvfb_log" >&2
        exit 1
    fi
    sleep 0.05
done

if [ ! -s "$display_file" ]; then
    if grep -q 'Owner of /tmp/.X11-unix should be set to root' "$xvfb_log"; then
        echo "Xvfb smoke test: skipped (sandbox does not permit X11 sockets)"
        exit 0
    fi
    echo "Xvfb smoke test: Xvfb failed to start" >&2
    sed -n '1,120p' "$xvfb_log" >&2
    exit 1
fi

display=":$(sed -n '1p' "$display_file")"
DISPLAY=$display "$binary" -c -f -F --refresh-rate 60 \
    >"$compositor_log" 2>&1 &
compositor_pid=$!
sleep 0.5

if ! kill -0 "$compositor_pid" 2>/dev/null; then
    echo "Xvfb smoke test: compositor exited during startup" >&2
    sed -n '1,160p' "$compositor_log" >&2
    exit 1
fi

if command -v xmessage >/dev/null 2>&1; then
    DISPLAY=$display xmessage "fastcompmgr smoke test" >/dev/null 2>&1 &
    client_pid=$!
    sleep 0.5
    kill "$client_pid" 2>/dev/null || :
    wait "$client_pid" 2>/dev/null || :
    client_pid=
    sleep 0.2
fi

if ! kill -0 "$compositor_pid" 2>/dev/null; then
    echo "Xvfb smoke test: compositor exited while handling a window" >&2
    sed -n '1,160p' "$compositor_log" >&2
    exit 1
fi

echo "Xvfb smoke test: passed"
