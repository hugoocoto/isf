#!/usr/bin/env bash
# What syncing asks of the remote, counted: a regression shows as a number
. "$(dirname "$0")/lib.sh"
count_requests

# New trees of 3 levels on both sides: 4 + 16 + 32 directories each
for a in 1 2 3 4; do for b in 1 2 3 4; do for c in 1 2; do
        mkdir -p "$L/a$a/b$b/c$c" "$R/x$a/y$b/z$c"
        echo "$a$b$c" >"$L/a$a/b$b/c$c/f"
        echo "$a$b$c" >"$R/x$a/y$b/z$c/f"
done; done; done
start
wait_same
stop
report() { echo "  $1: OPENDIR=$(requests OPENDIR) MKDIR=$(requests MKDIR) SETSTAT=$(requests SETSTAT) rounds=$(requests rounds), on the main connection $(main_requests rounds)"; }
report "first sync"
# The agent listed the remote tree as it started: nothing is listed over SFTP
[ "$(requests OPENDIR)" = 0 ] || fail "listed over SFTP: $(requests OPENDIR) folders"
# The folders made there go together, their modes too: not a round each
[ "$(main_requests rounds)" -le 12 ] || fail "too many rounds for the first sync: $(main_requests rounds)"
first_rounds=$(requests rounds)

requests_clear
start
expect_out "already in sync"
stop
report "in sync"
# Nothing to do, and nothing asked: the agent listed the remote tree as it
# started, so each connection only says hello (INIT)
[ "$(requests MKDIR)" = 0 ] && [ "$(requests SETSTAT)" = 0 ] || fail "changed something"
[ "$(requests OPENDIR)" = 0 ] || fail "listed over SFTP: $(requests OPENDIR) folders"
[ "$(requests LSTAT)" = 0 ] && [ "$(requests STAT)" = 0 ] || fail "the remote was asked about"
[ "$(main_requests rounds)" = 1 ] || fail "too many rounds to start: $(main_requests rounds)"
echo "  (first sync: $first_rounds rounds)"

# What isf does there itself comes back from the agent, and isn't taken for
# someone else's write: nothing is fetched back
start
echo more >"$L/a1/b1/c1/g"
wait_same
settle
requests_clear
mv "$L/a1/b1/c1/g" "$L/a1/b1/c1/h"
wait_same
settle
[ "$(requests READ)" = 0 ] || fail "a rename here was fetched back"
echo edited >"$L/a1/b1/c1/h"
wait_same
settle
[ "$(requests READ)" = 0 ] || fail "a file sent was fetched back"
chmod 600 "$R/x1/y1/z1/f"
wait_for '[ "$(stat -c %a "$L/x1/y1/z1/f")" = 600 ]'
settle
[ "$(requests READ)" = 0 ] || fail "a chmod there was fetched as content"

