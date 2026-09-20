#!/usr/bin/env bash
# Symlinks: synced as symlinks, whatever they point at, and replaced by (or
# replacing) files and folders without following them
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"

# The folder to sync, given as a symlink: what it points at is synced
mkdir -p "$T/local/real" && echo x >"$T/local/real/f"
ln -s real "$T/local/aslink"
echo there >"$T/home/linked/g" 2>/dev/null || mkdir -p "$T/home/linked" && echo there >"$T/home/linked/g"
(cd "$T/local" && exec timeout 20 "$ISF" ./aslink host:linked -I "$ISF" --once) >"$T/out" 2>"$T/err"
[ $? = 0 ] || fail "a folder given as a symlink didn't sync: $(head -2 "$T/err")"
expect_file "$T/home/linked/f" x
expect_file "$T/local/real/g" there

ln -s nowhere "$L/dangling"          # points at nothing
ln -s ../outside "$L/outside"        # out of the folder
ln -s real "$L/rel"                  # inside the folder
echo real >"$L/real"
ln -s loop2 "$L/loop1"               # a loop
ln -s loop1 "$L/loop2"
start
wait_same
settle
[ "$(readlink "$R/dangling")" = nowhere ] || fail "the dangling one wasn't sent"
[ "$(readlink "$R/loop1")" = loop2 ] || fail "the loop wasn't sent"
[ "$(readlink "$R/outside")" = ../outside ] || fail "the one out of the folder wasn't sent"
[ -L "$R/rel" ] && [ "$(cat "$R/rel")" = real ] || fail "the relative one doesn't point at the file"

# A symlink becomes a file there, and a file becomes a symlink here
rm "$R/dangling" && echo now-a-file >"$R/dangling"
rm "$L/real" && ln -s rel "$L/real"
wait_same
settle
[ -f "$L/dangling" ] && [ ! -L "$L/dangling" ] || fail "it's still a symlink here"
expect_file "$L/dangling" now-a-file
[ -L "$R/real" ] || fail "it's still a file there"

# A folder becomes a symlink there, with something in it here
mkdir -p "$L/d" && echo inside >"$L/d/f"
wait_for '[ -e "$R/d/f" ]'
settle
mkdir -p "$T/outside"
rm -r "$R/d" && ln -s "$T/outside" "$R/d"
wait_same
settle
[ -L "$L/d" ] && [ "$(readlink "$L/d")" = "$T/outside" ] || fail "the folder wasn't replaced here"
[ -z "$(ls -A "$T/outside")" ] || fail "it wrote into what the symlink points at: $(ls "$T/outside")"

# And a symlink becomes a folder with files in it, both ways
rm "$R/d" && mkdir "$R/d" && echo back >"$R/d/g"
wait_same
settle
expect_file "$L/d/g" back
[ -d "$L/d" ] && [ ! -L "$L/d" ] || fail "it's still a symlink here"
