#!/usr/bin/env bash
# The result doesn't depend on -j
. "$(dirname "$0")/lib.sh"

fill() { # DIR SEED: files in nested directories, some read-only
        for d in $(seq 8); do
                mkdir -p "$1/d$d/e"
                for f in $(seq 15); do
                        echo "$2 $d $f" >"$1/d$d/f$f"
                        echo "$2 $d $f" >"$1/d$d/e/g$f"
                done
                yes "$2 $d" | head -c $((d * 40000)) >"$1/d$d/big"
        done
        find "$1" -exec touch -h -d '2024-01-01 00:00:00' {} +
        chmod 555 "$1/d3/e"
        chmod 444 "$1/d4/f1"
}

for j in 1 4 8; do
        rm -rf "$L" "$R" "$XDG_STATE_HOME"/*
        mkdir -p "$L/up" "$R/down"
        fill "$L/up" up
        fill "$R/down" down
        start ./proj host:proj -j $j
        wait_same "-j $j"
        stop
        tree "$L" >"$T/tree-$j"
        chmod -R u+w "$L" "$R"
done
cmp -s "$T/tree-1" "$T/tree-4" || fail "-j 1 and -j 4 differ"
cmp -s "$T/tree-1" "$T/tree-8" || fail "-j 1 and -j 8 differ"
