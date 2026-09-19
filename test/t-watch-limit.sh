#!/usr/bin/env bash
# Out of inotify watches: one clear warning per side, and what can't be
# watched is still synced when isf starts
if [ -z "${ISF_TEST_IN_NS:-}" ]; then
        # In a user namespace of its own, where the limit can be made small
        unshare -Ur true 2>/dev/null || { echo "skipped: no user namespaces"; exit 0; }
        ISF_TEST_IN_NS=1 exec unshare -Ur bash "$0" "$@"
fi
echo 40 >/proc/sys/user/max_inotify_watches
. "$(dirname "$0")/lib.sh"

for i in $(seq 30); do mkdir -p "$L/l$i" "$R/r$i"; done
echo x >"$L/l30/f"
echo x >"$R/r30/f"
start
wait_same
expect_err "the limit of inotify watches"
[ "$(grep -c 'limit of inotify watches' "$T/err")" -le 2 ] || fail "warned more than once per side"
