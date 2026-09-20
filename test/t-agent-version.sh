#!/usr/bin/env bash
# Another isf on the remote, which speaks another agent protocol, is refused
# with what to do about it
. "$(dirname "$0")/lib.sh"

# From before versions: its ready message says nothing else
cat >"$T/old-isf" <<'END'
#!/bin/sh
printf 'R\000\000'
exec cat >/dev/null
END
# A newer one, protocol 99
cat >"$T/new-isf" <<'END'
#!/bin/sh
printf 'R\000'
printf '%016xv99\000' 99
exec cat >/dev/null
END
# One that speaks this protocol, but is another version: also refused, since
# the same number can mean something else in another build
cat >"$T/other-isf" <<END
#!/bin/sh
printf 'R\000'
printf '%016xv0.0-other\000' $(grep -o 'AGENT_PROTOCOL [0-9]*' "$TOP/src/agent.h" | grep -o '[0-9]*')
exec cat >/dev/null
END
chmod +x "$T/old-isf" "$T/new-isf" "$T/other-isf"

for agent in old new; do
        (cd "$T/local" && exec timeout 20 "$ISF" ./proj host:proj -I "$T/$agent-isf") >"$T/out" 2>"$T/err"
        [ $? != 0 ] || fail "synced with the $agent agent"
        expect_err "The same isf has to be on both sides"
        expect_err "scp $(readlink -f "$ISF") host:.local/bin/isf"
done
expect_err "isf on 'host' is v99, and this one is"
(cd "$T/local" && exec timeout 20 "$ISF" ./proj host:proj -I "$T/old-isf") >"$T/out" 2>"$T/err"
expect_err "isf on 'host' is an older version"

(cd "$T/local" && exec timeout 20 "$ISF" ./proj host:proj -I "$T/other-isf") >"$T/out" 2>"$T/err"
[ $? != 0 ] || fail "synced with another version of isf"
expect_err "isf on 'host' is v0.0-other"
expect_err "isf --update"
