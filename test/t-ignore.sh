#!/usr/bin/env bash
# .isfignore and editor temp files
. "$(dirname "$0")/lib.sh"

printf '# comment\n*.log\nbuild/\n/top.txt\ndocs/*.tmp\n' >"$L/.isfignore"
mkdir -p "$L/build" "$L/sub/build" "$L/docs" "$L/sub/docs"
echo x >"$L/a.log"
echo x >"$L/sub/b.log"
echo x >"$L/build/out"
echo x >"$L/sub/build/out"
echo x >"$L/top.txt"
echo x >"$L/sub/top.txt"
echo x >"$L/docs/n.tmp"
echo x >"$L/sub/docs/n.tmp"
echo x >"$L/.f.swp"
echo x >"$L/f~"
echo x >"$L/keep.txt"
mkdir -p "$R"
echo x >"$R/remote.log"

start
wait_for '[ -e "$R/keep.txt" ] && [ -e "$L/remote.log" ] || [ -e "$R/sub/top.txt" ]'
settle
for p in .isfignore keep.txt sub/top.txt sub/docs/n.tmp; do
        [ -e "$R/$p" ] || fail "$p should be synced"
done
for p in a.log sub/b.log build sub/build top.txt docs/n.tmp .f.swp f~; do
        expect_missing "$R/$p"
done
expect_missing "$L/remote.log"

# While running, both ways
echo x >"$L/new.log"
echo x >"$R/new2.log"
echo x >"$L/new.txt"
wait_for '[ -e "$R/new.txt" ]'
settle
expect_missing "$R/new.log"
expect_missing "$L/new2.log"

# Changing .isfignore applies right away, also to what changes with it
printf '*.txt\n' >"$L/.isfignore"
echo x >"$L/later.txt"
echo x >"$L/later.log"
wait_for '[ -e "$R/later.log" ]'
settle
expect_missing "$R/later.txt"

# Changed on the remote: what it doesn't ignore anymore is sent now
# (another size: a same-size edit in the same second can be missed, see README)
printf '*.log\n# remote\n' >"$R/.isfignore"
echo x >"$R/r.txt"
wait_for '[ -e "$L/r.txt" ] && [ -e "$R/later.txt" ]'

# .isfignore edited here while isf wasn't running: sent when it starts, and
# not taken back from what the remote had before
wait_for 'cmp -s "$L/.isfignore" "$R/.isfignore"'
settle # recorded
stop
printf '*.log\n# edited while stopped, and longer\n' >"$L/.isfignore"
start
wait_for 'grep -q "edited while stopped" "$R/.isfignore"'
settle
grep -q "edited while stopped" "$L/.isfignore" || fail "the edit was taken back"
grep -q "↓ .isfignore" "$T/out" && fail "the edit came back as received"
expect_out "in sync: 1 sent, 0 received"
