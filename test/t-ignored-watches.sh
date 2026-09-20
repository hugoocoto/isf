#!/usr/bin/env bash
# Ignored folders aren't watched, on either side: a big one (node_modules)
# would use thousands of watches, for events that are dropped anyway
. "$(dirname "$0")/lib.sh"

# How many folders a process watches, from its inotify fd
watches() { # PID
        grep -h '^inotify wd:' /proc/"$1"/fdinfo/* 2>/dev/null | wc -l
}

printf 'big/\n' >"$L/.isfignore"
for i in $(seq 40); do mkdir -p "$L/big/d$i/deeper"; done
mkdir -p "$L/kept/one" "$L/kept/two"
echo x >"$L/kept/one/f"
mkdir -p "$R"
start
wait_for '[ -e "$R/kept/one/f" ]'
settle
expect_missing "$R/big"
pid=$(pgrep -g "$PID" -x isf | head -1)
agent=$(for p in $(pgrep -g "$PID"); do grep -qa -- '--agent' "/proc/$p/cmdline" 2>/dev/null && echo "$p"; done | head -1)
[ -n "$pid" ] && [ -n "$agent" ] || fail "cannot find isf ($pid) or its agent ($agent)"
here=$(watches "$pid")
there=$(watches "$agent")
echo "  watches: $here here, $there there (80 ignored folders)"
[ "$here" -lt 20 ] || fail "it watched the ignored folders here: $here"
[ "$there" -lt 20 ] || fail "it watched the ignored folders there: $there"

# What stops being ignored is watched, without a restart, on both sides
printf '# nothing ignored now\n' >"$L/.isfignore"
WAIT=30 wait_for '[ -e "$R/big/d40/deeper" ]'
settle
echo new >"$L/big/d1/here"
echo new >"$R/big/d2/there"
WAIT=30 wait_for '[ -e "$R/big/d1/here" ] && [ -e "$L/big/d2/there" ]'
wait_same
[ "$(watches "$pid")" -gt 40 ] || fail "it didn't watch them once they stopped being ignored"
