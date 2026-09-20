#!/usr/bin/env bash
# A remote directory that can't be listed isn't taken for an empty one
. "$(dirname "$0")/lib.sh"
[ "$(id -u)" != 0 ] || { echo "skipped: root reads anything"; exit 0; }

mkdir -p "$L/d/sub"
echo keep >"$L/d/f"
echo keep >"$L/d/sub/g"
start
wait_same
stop

chmod 000 "$R/d"
trap 'chmod 755 "$R/d" "$L/d" "$R/ro" "$L/ro" 2>/dev/null; cleanup' EXIT
start
stop
chmod 755 "$L/d" # its mode was synced
expect_err "Cannot list 'host:proj/d': Permission denied"
expect_file "$L/d/f" keep
expect_file "$L/d/sub/g" keep
stop
chmod 755 "$R/d"

# Deeper down: the agent lists what's around it, not it (it can't), and isf
# doesn't take that for empty either
mkdir -p "$L/e/x/locked"
echo keep >"$L/e/x/locked/f"
start
wait_same
stop
chmod 000 "$R/e/x/locked"
trap 'chmod 755 "$R/d" "$L/d" "$R/e/x/locked" "$L/e/x/locked" "$R/ro" "$L/ro" 2>/dev/null; cleanup' EXIT
start
stop
chmod 755 "$L/e/x/locked"
expect_err "Cannot list 'host:proj/e/x/locked': Permission denied"
expect_file "$L/e/x/locked/f" keep
chmod 755 "$R/e/x/locked"

# A remote folder that can't be written into: what goes in it fails, nothing
# of it is recorded, and it's all sent once it can be
mkdir -p "$L/ro" && echo x >"$L/ro/old"
start
wait_same
stop
chmod 555 "$R/ro"
mkdir -p "$L/ro/new/deeper" && echo y >"$L/ro/new/deeper/f"
start
expect_err "Cannot create 'host:proj/ro/new'"
expect_out "not all in sync"
stop
chmod 755 "$R/ro" "$L/ro"
start
wait_same
expect_file "$R/ro/new/deeper/f" y

stop

# A remote folder that can be read but not looked into (mode 644): its names
# come back but nothing can be stat'ed, so a listing of it looks empty. What's
# here isn't deleted to match, and isf says why.
mkdir -p "$L/half" && echo one >"$L/half/a" && echo two >"$L/half/b"
run ./proj host:proj --once
chmod 644 "$R/half"
trap 'chmod 755 "$R/d" "$L/d" "$R/e/x/locked" "$L/e/x/locked" "$R/ro" "$L/ro" "$R/half" "$L/half" 2>/dev/null; cleanup' EXIT
run ./proj --once
[ "$STATUS" = 1 ] || fail "it didn't say something was wrong (exit $STATUS)"
expect_err "Cannot look inside"
chmod 755 "$L/half" # the mode there was synced here too
expect_file "$L/half/a" one
expect_file "$L/half/b" two

# Readable again there: what's in it syncs as usual
chmod 755 "$R/half"
echo three >"$R/half/c"
run ./proj --once
[ "$STATUS" = 0 ] || fail "it still couldn't sync: $(cat "$T/err")"
expect_file "$L/half/c" three
