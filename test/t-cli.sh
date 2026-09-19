#!/usr/bin/env bash
# Remembered destinations, several folders, odd names, the lock and the
# wiped-side check
. "$(dirname "$0")/lib.sh"

echo a >"$L/a"
start ./proj host:proj
wait_same
stop

# Remembered: no remote needed
echo b >"$L/b"
start ./proj
expect_out "./proj ⇄ host:proj"
wait_same
stop
(cd "$L" && exec setsid "$ISF" >"$T/out" 2>"$T/err") &
PID=$!
wait_for 'grep -q "watching for changes" "$T/out"'
expect_out ". ⇄ host:proj"
stop

# A second isf on the same folder is refused
start
run ./proj
[ "$STATUS" != 0 ] || fail "a second isf started"
expect_err "already being synced"
stop

# Wiped local side: stops instead of deleting the remote, --reset copies back
rm -f "$L"/*
run ./proj
[ "$STATUS" != 0 ] || fail "synced a wiped folder"
expect_err "is empty, but it was synced before"
[ -e "$R/a" ] || fail "the remote was changed"
start ./proj --reset
wait_same
expect_file "$L/a" a

# Unknown folder without a remote
mkdir "$T/local/other"
run ./other
[ "$STATUS" != 0 ] || fail "synced a folder without a destination"
expect_err "where does './other' sync to?"
stop

# Several folders go inside the remote folder, with their names
mkdir -p "$T/local/x" "$T/local/y z"
echo x >"$T/local/x/f"
echo y >"$T/local/y z/f"
start ./x "./y z" host:backups/
wait_for '[ -e "$T/home/backups/x/f" ] && [ -e "$T/home/backups/y z/f" ]'
echo x2 >"$T/local/x/g"
wait_for '[ -e "$T/home/backups/x/g" ]'
expect_out "x/g"
stop

# Names that need quoting on the remote shell, and odd file names
mkdir -p "$T/local/q"
echo q >"$T/local/q/-rf"
echo q >"$T/local/q/it's \"quoted\" \$(x) ñ"
start ./q "host:it's \$HOME"
wait_for '[ -e "$T/home/it'"'"'s \$HOME/-rf" ]'
echo live >"$T/home/it's \$HOME/from-remote"
wait_for '[ -e "$T/local/q/from-remote" ]'
L=$T/local/q R="$T/home/it's \$HOME" wait_same

# isf missing on the remote: says how to copy it there
(cd "$T/local" && exec timeout 20 "$ISF" ./proj host:proj -I /nowhere/isf) >"$T/out" 2>"$T/err"
expect_err "isf has to be installed on 'host' too"
expect_err "scp $(readlink -f "$ISF") host:.local/bin/isf"
