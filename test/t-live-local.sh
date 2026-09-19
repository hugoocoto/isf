#!/usr/bin/env bash
# Changes made locally while isf runs show up on the remote
. "$(dirname "$0")/lib.sh"

start

echo one >"$L/f.txt"
wait_same "create"
echo two >>"$L/f.txt"
wait_same "append"
chmod 640 "$L/f.txt"
wait_same "chmod"
touch -d '2019-05-05 05:05:05' "$L/f.txt"
wait_same "touch"

mkdir -p "$L/a/b/c"
echo deep >"$L/a/b/c/d.txt"
wait_same "mkdir -p"

mv "$L/f.txt" "$L/g.txt"
wait_same "rename file"
expect_out "f.txt → g.txt"
mv "$L/a" "$L/z"
wait_same "rename directory"
expect_out "a → z"

ln -s g.txt "$L/l"
wait_same "symlink"
ln -sfn z "$L/l"
wait_same "retarget symlink"

rm "$L/g.txt"
wait_same "remove file"
rm -r "$L/z"
wait_same "remove directory"

# A file replaced by a directory, and back
echo f >"$L/x"
wait_same
rm "$L/x" && mkdir "$L/x" && echo in >"$L/x/in.txt"
wait_same "file to directory"
rm -r "$L/x" && echo f2 >"$L/x"
wait_same "directory to file"

# A burst of changes, including a tree copied in
mkdir "$T/src"
for i in $(seq 50); do echo "$i" >"$T/src/$i"; done
cp -r "$T/src" "$L/copied"
for i in $(seq 20); do echo "$i" >"$L/n$i"; done
wait_same "burst"

# Moved out of the folder: deleted on the remote
mv "$L/copied" "$T/away"
wait_same "moved out"
# Moved in: sent
mv "$T/away" "$L/back"
wait_same "moved in"

# A synced folder replaced by another of the same name: what's in the new one
# has no events of its own, and is sent all the same
mkdir -p "$L/swap" && echo old >"$L/swap/old"
wait_same "a folder to swap"
mkdir -p "$T/other" && echo new >"$T/other/new"
rm -r "$L/swap" && mv "$T/other" "$L/swap"
wait_same "a folder swapped for another"
expect_file "$R/swap/new" new
