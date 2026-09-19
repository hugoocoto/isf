#!/usr/bin/env bash
# Run the tests: test/run.sh [t-name...] (default: all). Each test syncs
# folders under $TMPDIR through test/fake-ssh, so no server is needed.

cd "$(dirname "$0")" || exit 2
[ $# -gt 0 ] || set -- t-*.sh
failed=0
for t in "$@"; do
        t=${t%.sh}
        start=$EPOCHREALTIME
        if out=$(bash "./$t.sh" 2>&1); then
                printf 'ok    %-16s %5.1fs\n' "$t" "$(awk "BEGIN { print $EPOCHREALTIME - $start }")"
        else
                printf 'FAIL  %s\n%s\n' "$t" "$out"
                failed=1
        fi
done
exit $failed
