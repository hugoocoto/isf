#!/usr/bin/env bash
# Thousands of directories side by side: listed in batches that fit the pipes
. "$(dirname "$0")/lib.sh"

for i in $(seq 2000); do mkdir -p "$R/wide/some-longer-directory-name-$i/sub"; done
for i in $(seq 0 100 2000); do echo "$i" >"$R/wide/some-longer-directory-name-$i/sub/f"; done
start
wait_same
stop
start
expect_out "already in sync"
