#!/usr/bin/env bash
# A file isf just sent is edited there before isf read the report of its own
# write, with the same size and mtime: the edit isn't taken for isf's own
. "$(dirname "$0")/lib.sh"

export ISF_TEST_LATENCY_MS=100
WAIT=60
start

# A small file sent with a big one: it's there long before the flush ends,
# and the reports of what happens to it wait until then
yes big | head -c $((16 << 20)) >"$T/big"
printf 'gggg' >"$T/g"
touch -d @1700000000 "$T/g"
mv "$T/big" "$T/g" "$L/"
wait_for '[ -e "$R/g" ]'
printf 'hhhh' >"$R/g"
touch -d @1700000000 "$R/g"
wait_same
expect_file "$L/g" hhhh
