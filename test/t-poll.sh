#!/usr/bin/env bash
# A remote folder on a network filesystem is walked every so often, for what
# other machines change in it: inotify never hears of that. A write through
# mmap stands in for one here (inotify doesn't report those either), and
# ISF_TEST_POLL_MS walks the test's folder as if it were on one.
. "$(dirname "$0")/lib.sh"
export ISF_TEST_POLL_MS=300

# Map FILE, close it (an event), then write TEXT through the mapping after a
# second and a bit: a new mtime, the same size, and no event. The mapping is
# dropped only once $T/release exists: that is an event too.
mmap_write() {
        python3 -c '
import mmap, os, sys, time
with open(sys.argv[1], "r+b") as f:
    m = mmap.mmap(f.fileno(), 0)
time.sleep(1.2)
m[:len(sys.argv[2])] = sys.argv[2].encode()
m.flush()
end = time.time() + 60
while not os.path.exists(sys.argv[3]) and time.time() < end:
    time.sleep(0.05)
m.close()
' "$1" "$2" "$T/release"
}

mkdir -p "$R"
echo aaaa >"$R/f"
mkdir "$R/d"
echo in >"$R/d/in"
start
wait_same

mmap_write "$R/f" bbbb &
WRITER=$!
wait_for '[ "$(cat "$L/f" 2>/dev/null)" = bbbb ]'
wait_same "a change no event reported"
touch "$T/release"
wait "$WRITER"

# isf's own uploads come back as events: the walks must not take them for
# someone else's writes (that would make conflict copies)
for i in $(seq 20); do
        echo "$i" >"$L/up$i"
        echo "$i" >>"$L/d/in"
done
wait_same "uploads while walking"
sleep 1 # a few walks
find "$L" "$R" -name '*.isf-conflict' | grep -q . && fail "conflict copies"

# Made and removed on the remote: still reported once, by the events
mkdir -p "$R/n/m"
echo x >"$R/n/m/x"
wait_same "made on the remote"
rm -r "$R/n" "$R/up1"
wait_same "removed on the remote"
sleep 1
find "$L" "$R" -name '*.isf-conflict' | grep -q . && fail "conflict copies"
[ ! -s "$T/err" ] || fail "errors"
