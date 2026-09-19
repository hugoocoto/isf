#!/usr/bin/env bash
# Peak memory (VmHWM) of each process isf runs, on both sides, through
# test/fake-ssh:
#
#   test/mem.sh FILES DIRS SIDE
#
# FILES are spread over DIRS directories on SIDE (local or remote). Prints the
# peaks after the first sync, and after a restart that finds it all in sync.
. "$(dirname "$0")/lib.sh"
files=$1 dirs=$2 side=$3
base=$([ "$side" = remote ] && echo "$R" || echo "$L")
for d in $(seq "$dirs"); do mkdir -p "$base/d$d"; done
for i in $(seq 0 $((files - 1))); do echo "file $i" >"$base/d$(( i % dirs + 1 ))/f$i"; done
echo "$files files in $dirs dirs, on the $side side"
peak() { # the processes of isf's group: name, peak RSS
        for p in $(pgrep -g "$PID"); do
                n=$(tr '\0' ' ' </proc/$p/cmdline | cut -c1-40)
                h=$(awk '/VmHWM/ { print $2 }' /proc/$p/status)
                printf '  %-40s %7d KB\n' "$n" "$h"
        done
}
start ./proj host:proj
echo "after the first sync:"; peak
stop
start ./proj host:proj
echo "restarted, in sync:"; peak
stop
