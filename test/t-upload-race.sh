#!/usr/bin/env bash
# A file written on the remote while isf is sending one over it: the agent
# checks and renames there, with nothing in between, so the write isn't lost
. "$(dirname "$0")/lib.sh"

export ISF_TEST_LATENCY_MS=1000 # a second between isf's requests and the remote
count_requests
WAIT=60

echo first >"$L/f"
start
wait_same
settle

# Edit here (it goes over) and there (once the data is on its way): the one
# made there wins or is kept, but never disappears
requests_clear
printf 'a long line from here\n' >"$L/f"
wait_for '[ "$(requests WRITE)" -ge 1 ]' # the data and the check are on their way
# The check reaches the remote half a round trip later, and what isf sends
# after it another one: this lands in between, where it used to be lost
sleep 0.8
echo "from there" >"$R/f"
WAIT=60 wait_same
settle
if [ "$(cat "$R/f")" = "from there" ]; then
        : # it stayed, and came here
elif [ -e "$R/f.isf-conflict" ] || [ -e "$L/f.isf-conflict" ]; then
        grep -rq "from there" "$L" || fail "the write made there is gone"
else
        fail "the write made there is gone: remote has '$(cat "$R/f")'"
fi
grep -rq "from there" "$L" || fail "the write made there never arrived here"
