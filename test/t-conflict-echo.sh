#!/usr/bin/env bash
# isf's own doing, reported back to it while it syncs, mustn't undo the step
# it took after it: a folder here against a file there keeps that file aside
# as a conflict copy, and the agent's report of that rename ("the file is
# gone") reaches isf before its report of the folder isf then made there.
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
echo hello >"$L/a" # something to send: that's when isf reads what it says
mkdir "$L/f"       # a folder here, a file there: the folder wins
: >"$R/f"

ISF_TEST_PROTOCOL=$(grep -o 'AGENT_PROTOCOL [0-9]*' "$TOP/src/agent.h" | grep -o '[0-9]*')
ISF_TEST_VERSION=$("$ISF" --version | cut -d' ' -f2-)
export ISF_TEST_PROTOCOL ISF_TEST_VERSION
export ISF_TEST_SAY_MOVED=f:f.isf-conflict
AGENT=$TOP/test/fake-agent.py

run ./proj host:proj --once
[ "$STATUS" = 0 ] || fail "isf failed: $(cat "$T/err")"
[ -d "$L/f" ] || fail "the folder here was deleted"
[ -d "$R/f" ] || fail "the folder isn't there"
expect_file "$R/a" hello
expect_file "$L/f.isf-conflict" ""
wait_same
