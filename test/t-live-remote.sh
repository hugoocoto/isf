#!/usr/bin/env bash
# Changes made on the remote while isf runs show up locally, through the agent
. "$(dirname "$0")/lib.sh"

start

echo one >"$R/f.txt"
wait_same "create"
echo two >>"$R/f.txt"
wait_same "append"
chmod 640 "$R/f.txt"
wait_same "chmod"

mkdir -p "$R/a/b/c"
echo deep >"$R/a/b/c/d.txt"
wait_same "mkdir -p"

mv "$R/f.txt" "$R/g.txt"
wait_same "rename file"
mv "$R/a" "$R/z"
wait_same "rename directory"

ln -s g.txt "$R/l"
wait_same "symlink"

rm "$R/g.txt"
wait_same "remove file"
rm -r "$R/z"
wait_same "remove directory"

echo f >"$R/x"
wait_same
rm "$R/x" && mkdir "$R/x" && echo in >"$R/x/in.txt"
wait_same "file to directory"

mkdir "$T/src"
for i in $(seq 50); do echo "$i" >"$T/src/$i"; done
cp -r "$T/src" "$R/copied"
wait_same "tree copied in"

# The remote folder removed: isf stops instead of deleting everything here
rm -rf "$R"
wait_for '! kill -0 $PID 2>/dev/null'
PID=
[ -e "$L/copied/1" ] || fail "local files were removed"
