#!/usr/bin/env bash
# Transfer connections that can't be opened: isf goes on with the one it has
. "$(dirname "$0")/lib.sh"

mkdir -p "$L/d" "$R/e"
for i in $(seq 30); do echo "$i" >"$L/d/$i"; echo "$i" >"$R/e/$i"; done
export ISF_TEST_SFTP_LIMIT=2 # the main one and one of the four
start
wait_same
expect_err "Using a single connection instead of 4"
echo more >"$L/d/more"
wait_same
