#!/usr/bin/env bash
# isf killed in the middle of syncing, again and again: what it was doing is
# picked up when it starts again, and the two sides still come together
. "$(dirname "$0")/lib.sh"

SEED=${SEED:-5}
ROUNDS=${ROUNDS:-8}
RANDOM=$SEED
WAIT=60
export ISF_TEST_LATENCY_MS=${LAT:-20}
mkdir -p "$R"

# What both sides have, ignoring the temp files of a transfer that was killed
kept() { tree "$1" | grep -v '\.isf\.[0-9]*\.[0-9]*\.tmp'; }

size=0
write() { # FILE ROUND
        size=$((size + 1))
        mkdir -p "$(dirname "$1")"
        head -c $((size * 1000)) /dev/zero | tr '\0' "$2" >"$1"
}

for round in $(seq "$ROUNDS"); do
        # Something to do on both sides, then killed while it does it
        for side in "$L" "$R"; do
                write "$side/f$round" "$round"
                write "$side/d/g$round" "$round"
                [ -e "$side/f$((round - 1))" ] && rm -f "$side/f$((round - 1))"
        done
        (cd "$T/local" && exec setsid "$ISF" ./proj host:proj -I "$ISF" >>"$T/out" 2>>"$T/err") &
        PID=$!
        sleep "0.$((RANDOM % 5 + 2))" # somewhere in the middle of its work
        kill -KILL -- "-$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
        PID=
done

# Now let it finish
run ./proj host:proj --once
[ "$STATUS" = 0 ] || fail "it couldn't finish: $(tail -3 "$T/err")"
if [ "$(kept "$L")" != "$(kept "$R")" ]; then
        diff <(kept "$L") <(kept "$R") >&2
        fail "the two sides differ after $ROUNDS kills (SEED=$SEED)"
fi
# Nothing of isf's own is left over in what it synced
[ -z "$(kept "$L" | grep isf-conflict)" ] || echo "  (conflict copies were kept, which is fine)"
echo "  $ROUNDS kills, seed $SEED: they came together"
