#!/usr/bin/env bash
# Time how long isf takes to bring a tree in sync, through test/fake-ssh:
#
#   test/bench.sh SHAPE FILES [RTT_MS] [SIDE]
#
# SHAPE is wide (8 dirs of 9 subdirs), deep (8 chains of 10 nested dirs) or
# flat (dirs of 100 files). FILES are spread over the directories, on SIDE:
# local (the default: the first sync uploads them) or remote (it downloads
# them). RTT_MS delays every SFTP message by that round trip. Prints the time
# of the first sync, how long a change made right after it takes to get there
# (isf may still be busy with the first sync's own events), and the time of a
# second start, when everything is in sync already and isf only compares the
# two sides.

. "$(dirname "$0")/lib.sh"

shape=${1:-wide} files=${2:-400} rtt=${3:-0} side=${4:-local}
base=$([ "$side" = remote ] && echo "$R" || echo "$L")
mkdir -p "$base"
case $shape in
wide) for a in $(seq 8); do for b in $(seq 9); do mkdir -p "$base/a$a/b$b"; done; done ;;
deep) for a in $(seq 8); do p=$base/c$a; for d in $(seq 10); do p=$p/d$d; done; mkdir -p "$p"; done ;;
flat) for a in $(seq $(((files + 99) / 100))); do mkdir -p "$base/f$a"; done ;;
*) echo "bench: unknown shape '$shape'" >&2; exit 2 ;;
esac
mapfile -t dirs < <(find "$base" -type d)
for i in $(seq 0 $((files - 1))); do
        echo "$i" >"${dirs[i % ${#dirs[@]}]}/file$i"
done
echo "$shape: $files files, ${#dirs[@]} dirs on the $side side, rtt ${rtt} ms"

timed_start() {
        local t0=$EPOCHREALTIME
        (cd "$T/local" && ISF_TEST_LATENCY_MS=$rtt exec setsid "$ISF" ./proj host:proj -I "$ISF" \
                >"$T/out" 2>"$T/err") &
        PID=$!
        until grep -q "watching for changes" "$T/out"; do
                kill -0 "$PID" 2>/dev/null || { PID=; fail "isf exited"; }
                sleep 0.01
        done
        awk "BEGIN { printf \"%s %7.0f ms\n\", \"$1\", ($EPOCHREALTIME - $t0) * 1000 }"
}

[ "$rtt" = 0 ] && rtt=
timed_start "first sync:"
t0=$EPOCHREALTIME
echo probe >"$L/probe"
WAIT=600 wait_for '[ -e "$R/probe" ]'
awk "BEGIN { printf \"then a change: %7.0f ms\n\", ($EPOCHREALTIME - $t0) * 1000 }"
stop
[ "$(tree "$L")" = "$(tree "$R")" ] || fail "local and remote differ"
timed_start "in sync:   "
stop
