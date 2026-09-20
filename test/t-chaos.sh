#!/usr/bin/env bash
# Random changes on both sides, synced with --once: after each round the two
# sides have to be the same. SEED=n runs a particular round again.
. "$(dirname "$0")/lib.sh"

SEED=${SEED:-7}
ROUNDS=${ROUNDS:-12}
RANDOM=$SEED
mkdir -p "$R"
names=(a b c d e f g dir/x dir/y dir/z deep/one/two deep/one/three)
log=$T/chaos.log

# Every write has its own length: a change that keeps both the size and the
# second is the one thing isf can miss offline (see README)
# Each write has a size of its own: two writes of the same size in the same
# second are the one thing isf can't tell apart offline (see the README)
size=0
write() { # FILE ROUND
        size=$((size + 1))
        mkdir -p "$(dirname "$1")"
        head -c "$size" /dev/zero | tr '\0' "$2" >"$1"
}

for round in $(seq "$ROUNDS"); do
        for side in "$L" "$R"; do
                for i in 1 2 3; do
                        name=${names[$((RANDOM % ${#names[@]}))]}
                        path=$side/$name
                        case $((RANDOM % 9)) in
                        0 | 1) write "$path" "$round"; echo "round $round: write $path" >>"$log" ;;
                        2) rm -rf "$path" 2>/dev/null; echo "round $round: remove $path" >>"$log" ;;
                        3) mkdir -p "$path" 2>/dev/null && echo "round $round: mkdir $path" >>"$log" ;;
                        4) [ -e "$path" ] && mv "$path" "$path.moved" 2>/dev/null &&
                                echo "round $round: move $path" >>"$log" ;;
                        5) if [ -d "$path" ]; then mode=$((RANDOM % 2 ? 755 : 700)); else mode=$((RANDOM % 2 ? 644 : 600)); fi
                                [ -e "$path" ] && chmod "$mode" "$path" 2>/dev/null &&
                                        echo "round $round: chmod $mode $path" >>"$log" ;;
                        6) mkdir -p "$(dirname "$path")" && rm -rf "$path" 2>/dev/null
                                ln -s "../$((RANDOM % 5))-target" "$path" 2>/dev/null &&
                                        echo "round $round: symlink $path" >>"$log" ;;
                        7) mkdir -p "$(dirname "$path")" && rm -rf "$path" 2>/dev/null
                                : >"$path" && echo "round $round: empty $path" >>"$log" ;;
                        8) [ -e "$path" ] && touch "$path" && echo "round $round: touch $path" >>"$log" ;;
                        esac
                done
        done
        run ./proj host:proj --once
        [ "$STATUS" = 0 ] || { cat "$log" >&2; fail "round $round: isf failed: $(cat "$T/err")"; }
        if [ "$(tree "$L")" != "$(tree "$R")" ]; then
                diff <(tree "$L") <(tree "$R") >&2
                cat "$log" >&2
                fail "round $round (SEED=$SEED): the two sides differ"
        fi
done
echo "  $ROUNDS rounds, seed $SEED: the same on both sides every time"
