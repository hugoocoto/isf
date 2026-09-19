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
