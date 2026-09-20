#!/usr/bin/env bash
# The remote says what time it is there when it starts: clocks that don't
# agree are said, since they decide which side changed a file last
. "$(dirname "$0")/lib.sh"

version=$("$ISF" --version | cut -d' ' -f2-)
protocol=$(grep -o 'AGENT_PROTOCOL [0-9]*' "$TOP/src/agent.h" | grep -o '[0-9]*')

# An agent like the real one, but with a clock 10 minutes ahead (it watches
# nothing: isf then lists the folder itself)
agent() { # SKEW
        cat >"$T/skewed-isf" <<END
#!/bin/sh
mkdir -p "\$ISF_TEST_REMOTE/proj"
printf 'T\\000'
printf '%016x\\000' \$(( \$(date +%s) + $1 ))
printf 'R\\000'
printf '%016x$version\\000' $protocol
exec cat >/dev/null
END
        chmod +x "$T/skewed-isf"
}

# isf in the background with that agent (start() passes its own -I)
with_agent() {
        : >"$T/out"
        : >"$T/err"
        (cd "$T/local" && exec setsid "$ISF" ./proj host:proj -I "$T/skewed-isf" >"$T/out" 2>"$T/err") &
        PID=$!
        WAIT=20 wait_for 'grep -q "watching for changes" "$T/out"'
}

echo hello >"$L/f"
agent 600
with_agent
wait_same
expect_err "The clock on 'host' is 600 seconds ahead of this one"
expect_err "NTP"
stop

# In time: nothing said about it
agent 0
with_agent
wait_same
grep -q "clock" "$T/err" && fail "said something about clocks that agree"
stop
