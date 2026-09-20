#!/usr/bin/env bash
# --check-update and --update, with a curl that answers like GitHub
. "$(dirname "$0")/lib.sh"

cat >"$T/bin/curl" <<'END'
#!/bin/sh
# The arguments isf uses: -o FILE (or -) and the URL last
dest=-; url=
while [ $# -gt 0 ]; do
        case $1 in
        -o) dest=$2; shift 2 ;;
        -*) shift ;;
        *) url=$1; shift ;;
        esac
done
case $url in
*/releases/latest) body='{"tag_name":"'"$FAKE_TAG"'","name":"isf"}' ;;
*/git/ref/tags/nightly) body='{"ref":"refs/tags/nightly","object":{"sha":"'"$FAKE_SHA"'","type":"commit"}}' ;;
*/releases/download/*) [ -n "$FAKE_ASSET" ] || exit 22
        if [ "$dest" = - ]; then cat "$FAKE_ASSET"; else cp "$FAKE_ASSET" "$dest"; fi
        exit 0 ;;
*) exit 22 ;;
esac
if [ "$dest" = - ]; then printf '%s' "$body"; else printf '%s' "$body" >"$dest"; fi
END
chmod +x "$T/bin/curl"
export FAKE_TAG=v9.9.9 FAKE_SHA=0123456789abcdef0123456789abcdef01234567 FAKE_ASSET=

# An isf that says it is v9.9.9, like a release build
CFLAGS=-O0 make -s -C "$TOP" OUT="$T/bin/isf-v9" VERSION=v9.9.9 >/dev/null || fail "cannot build"
"$T/bin/isf-v9" --check-update >"$T/out" 2>"$T/err"
[ $? = 0 ] || fail "the newest one didn't say so (exit status)"
expect_out "v9.9.9 is the newest there is"

# A newer one there
export FAKE_TAG=v9.9.10
"$T/bin/isf-v9" --check-update >"$T/out" 2>"$T/err"
[ $? = 1 ] || fail "a newer one didn't say so (exit status)"
expect_out "this is isf v9.9.9; v9.9.10 is newer"
expect_out "isf --update"

# Nothing newer: --update leaves it alone and says so
export FAKE_TAG=v9.9.9
cp "$T/bin/isf-v9" "$T/bin/isf-same"
"$T/bin/isf-same" --update >"$T/out" 2>"$T/err"
[ $? = 0 ] || fail "--update on the newest one failed"
expect_out "is the newest there is"
cmp -s "$T/bin/isf-same" "$T/bin/isf-v9" || fail "it replaced itself with nothing newer"
export FAKE_TAG=v9.9.10

# --update takes it: the file is replaced, and what it replaces still runs
cp "$T/bin/isf-v9" "$T/bin/isf-new"
printf 'a newer isf' >>"$T/bin/isf-new" # tells them apart; ELF ignores the tail
export FAKE_ASSET=$T/bin/isf-new
cp "$T/bin/isf-v9" "$T/bin/isf-here"
"$T/bin/isf-here" --update >"$T/out" 2>"$T/err"
[ $? = 0 ] || fail "the update said it failed"
expect_out "updated"
grep -q 'a newer isf' "$T/bin/isf-here" || fail "it wasn't replaced"
"$T/bin/isf-here" --version | grep -q '^isf ' || fail "what it left doesn't run"

# A download that fails leaves it alone, and says so
cp "$T/bin/isf-v9" "$T/bin/isf-keep"
FAKE_ASSET= "$T/bin/isf-keep" --update >"$T/out" 2>"$T/err"
[ $? = 2 ] || fail "a failed update looked fine"
expect_err "is untouched"
cmp -s "$T/bin/isf-keep" "$T/bin/isf-v9" || fail "it was changed anyway"
[ -z "$(ls "$T/bin"/.isf.*.tmp 2>/dev/null)" ] || fail "a temp file was left"

# Something that isn't isf isn't put in its place either
printf 'not an isf at all' >"$T/bin/junk"
cp "$T/bin/isf-v9" "$T/bin/isf-keep2"
FAKE_ASSET=$T/bin/junk "$T/bin/isf-keep2" --update >"$T/out" 2>"$T/err"
[ $? = 2 ] || fail "junk was taken"
cmp -s "$T/bin/isf-keep2" "$T/bin/isf-v9" || fail "junk replaced it"
