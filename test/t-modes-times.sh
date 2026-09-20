#!/usr/bin/env bash
# Modes, times and files that can't be read
. "$(dirname "$0")/lib.sh"
[ "$(id -u)" != 0 ] || { echo "skipped: root reads anything"; exit 0; }

mkdir -p "$R"
: >"$L/empty"
printf 'x\n' >"$L/script"
chmod 755 "$L/script"
printf 'x\n' >"$L/setuid"
chmod 4755 "$L/setuid"
printf 'x\n' >"$L/sticky-dir-file"
mkdir -p "$L/sticky"
chmod 1777 "$L/sticky"
printf 'x\n' >"$L/future"
touch -d '2040-01-01 12:00:00' "$L/future"
printf 'x\n' >"$L/old"
touch -d '1980-03-04 05:06:07' "$L/old"
start
wait_same
settle
[ -s "$R/empty" ] && fail "the empty one isn't empty there"
[ -e "$R/empty" ] || fail "the empty one wasn't sent"
[ "$(stat -c %a "$R/script")" = 755 ] || fail "the mode wasn't sent: $(stat -c %a "$R/script")"
[ "$(stat -c %a "$R/setuid")" = 4755 ] || fail "setuid wasn't sent: $(stat -c %a "$R/setuid")"
[ "$(stat -c %a "$R/sticky")" = 1777 ] || fail "the sticky folder's mode wasn't sent: $(stat -c %a "$R/sticky")"
[ "$(stat -c %Y "$R/future")" = "$(stat -c %Y "$L/future")" ] || fail "the 2040 time wasn't sent"
[ "$(stat -c %Y "$R/old")" = "$(stat -c %Y "$L/old")" ] || fail "the 1980 time wasn't sent"

# A local file that can't be read: an error, and the rest still syncs
printf 'secret\n' >"$L/locked"
chmod 000 "$L/locked"
trap 'chmod 644 "$L/locked" 2>/dev/null; cleanup' EXIT
printf 'x\n' >"$L/after-the-locked-one"
wait_for '[ -e "$R/after-the-locked-one" ]'
settle
expect_missing "$R/locked"
expect_err "Cannot open"

# Readable again: it goes
chmod 644 "$L/locked"
touch "$L/locked"
wait_for '[ -e "$R/locked" ]'
expect_file "$R/locked" secret

# A mode changed on each side
chmod 700 "$L/script"
chmod 600 "$R/empty"
wait_for '[ "$(stat -c %a "$R/script")" = 700 ] && [ "$(stat -c %a "$L/empty")" = 600 ]'
wait_same
