#!/usr/bin/env bash
# --dry-run shows what a sync would do and changes nothing
. "$(dirname "$0")/lib.sh"

mkdir -p "$L/d" "$R"
echo a >"$L/a"
echo f >"$L/d/f"
echo r >"$R/r"
echo old >"$L/both"
echo new >"$R/both"
touch -d '2020-01-01 00:00:00' "$L/both"
touch -d '2021-01-01 00:00:00' "$R/both"
before=$(tree "$L"; echo; tree "$R")

run ./proj host:proj -n
[ "$STATUS" = 0 ] || fail "the dry run failed"
for line in "↑ a" "↑ d/" "↑ d/f" "↓ r" "↓ both" "! both changed on both sides" \
        "dry run, nothing was changed: 3 to send, 2 to receive, 1 conflict"; do
        expect_out "$line"
done
[ "$(tree "$L"; echo; tree "$R")" = "$before" ] || fail "the dry run changed something"

# Nothing was remembered either: not where it goes, not what was synced
run ./proj -n
expect_err "where does './proj' sync to?"

# A remote folder that isn't there isn't made
mkdir "$T/local/new"
echo x >"$T/local/new/x"
run ./new host:new -n
expect_out "↑ x"
[ ! -e "$T/home/new" ] || fail "the dry run made the remote folder"

# The real sync does what the dry run said
start
wait_same
expect_out "1 conflict"
