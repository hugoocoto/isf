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
trap 'chmod 755 "$R/d" "$L/d" 2>/dev/null; cleanup' EXIT
start
stop
chmod 755 "$L/d" # its mode was synced
expect_err "Cannot list 'host:proj/d': Permission denied"
expect_file "$L/d/f" keep
expect_file "$L/d/sub/g" keep
