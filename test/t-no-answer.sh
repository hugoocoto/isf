#!/usr/bin/env bash
# An isf on the remote that says hello and then nothing (a wedged one) must
# not stop this one: it puts what it sent in place itself
. "$(dirname "$0")/lib.sh"

CFLAGS='-O0 -DPLACE_WAIT_MS=400' make -s -C "$TOP" OUT="$T/bin/isf-quick" >/dev/null || fail "cannot build"
ISF=$T/bin/isf-quick
version=$("$ISF" --version | cut -d' ' -f2-)
protocol=$(grep -o 'AGENT_PROTOCOL [0-9]*' "$TOP/src/agent.h" | grep -o '[0-9]*')
cat >"$T/quiet-isf" <<END
#!/bin/sh
mkdir -p "\$ISF_TEST_REMOTE/proj"
printf 'R\000'
printf '%016x$version\000' $protocol
cat >/dev/null # not exec: its end of the pipe stays open, it just says nothing
END
chmod +x "$T/quiet-isf"

echo hello >"$L/f"
echo more >"$L/g"
(cd "$T/local" && exec setsid "$ISF" ./proj host:proj -I "$T/quiet-isf" >"$T/out" 2>"$T/err") &
PID=$!
WAIT=30 wait_for 'grep -q "watching for changes" "$T/out"'
expect_err "isn't answering"
expect_file "$R/f" hello
expect_file "$R/g" more
