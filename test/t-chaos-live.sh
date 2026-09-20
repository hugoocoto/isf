#!/usr/bin/env bash
# Random changes on both sides while isf watches: the two sides have to come
# together after each round. SEED=n ROUNDS=n to run a particular one again.
. "$(dirname "$0")/lib.sh"

SEED=${SEED:-3}
ROUNDS=${ROUNDS:-10}
RANDOM=$SEED
WAIT=60
[ -n "${LAT:-}" ] && export ISF_TEST_LATENCY_MS=$LAT # changes land mid-transfer
mkdir -p "$R"
names=(a b c d dir/x dir/y deep/one/two deep/one/three)
log=$T/chaos.log

# Each write has a size of its own: two writes of the same size in the same
# second are the one thing isf can't tell apart offline (see the README)
size=0
write() { # FILE ROUND
        size=$((size + 1))
        mkdir -p "$(dirname "$1")"
        head -c "$size" /dev/zero | tr '\0' "$2" >"$1"
}

start
for round in $(seq "$ROUNDS"); do
        for side in "$L" "$R"; do
                for i in 1 2 3; do
                        name=${names[$((RANDOM % ${#names[@]}))]}
                        path=$side/$name
                        case $((RANDOM % 8)) in
                        0 | 1) write "$path" "$round"; echo "round $round: write $path" >>"$log" ;;
                        2) rm -rf "$path" 2>/dev/null; echo "round $round: remove $path" >>"$log" ;;
                        3) mkdir -p "$path" 2>/dev/null && echo "round $round: mkdir $path" >>"$log" ;;
                        4) [ -e "$path" ] && mv "$path" "$path.moved" 2>/dev/null &&
                                echo "round $round: move $path" >>"$log" ;;
                        5) [ -e "$path" ] && chmod "$((RANDOM % 2 ? 755 : 644))" "$path" 2>/dev/null &&
                                echo "round $round: chmod $path" >>"$log" ;;
                        6) mkdir -p "$(dirname "$path")" && rm -rf "$path" 2>/dev/null
                                ln -s "../$((RANDOM % 5))-target" "$path" 2>/dev/null &&
                                        echo "round $round: symlink $path" >>"$log" ;;
                        7) mkdir -p "$(dirname "$path")" && rm -rf "$path" 2>/dev/null
                                : >"$path" && echo "round $round: empty $path" >>"$log" ;;
                        esac
                done
        done
        if ! wait_same "round $round (SEED=$SEED)" 2>/dev/null; then
                diff <(tree "$L") <(tree "$R") >&2
                cat "$log" >&2
                fail "round $round (SEED=$SEED): they never came together"
        fi
done
settle
wait_same "after settling"
echo "  $ROUNDS rounds, seed $SEED: they came together every time"
