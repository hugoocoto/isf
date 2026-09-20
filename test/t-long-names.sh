#!/usr/bin/env bash
# Names as long as a filesystem takes, and paths deep enough to be near what
# a path can hold
. "$(dirname "$0")/lib.sh"
WAIT=60

long=$(head -c 253 /dev/zero | tr '\0' 'n')  # 253, so "$long.d" is the longest a name can be (255)
mkdir -p "$R"
echo x >"$L/$long"
mkdir -p "$L/$long.d"
echo y >"$L/$long.d/$long"

# Deep: 30 levels of 100 characters is about 3000 of a path's 4096
deep=$L
part=$(head -c 100 /dev/zero | tr '\0' 'd')
for i in $(seq 30); do deep=$deep/$part; done
mkdir -p "$deep"
echo deep >"$deep/f"

start
wait_same
settle
expect_file "$R/$long" x
expect_file "$R/$long.d/$long" y
[ -e "${deep/$L/$R}/f" ] || fail "the deep one didn't arrive"

# Changed on the other side, at both ends
echo changed >"$R/$long"
echo changed >"${deep/$L/$R}/f"
wait_for 'grep -q changed "$L/'"$long"'"'
wait_same
settle
expect_file "$L/$long" changed
