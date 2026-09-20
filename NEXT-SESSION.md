# isf — what's left, and ideas

Notes to resume from. (This file isn't part of the build; delete it whenever.)
As always: don't commit, `git add` or `git push` anything; show the commands.

Round 4 is pushed (8831fa8), with the release-date fix after it (758f0cd),
both green on CI. What came after that isn't pushed yet:
- **a folder on the remote that can be read but not looked into (mode 644)
  listed as empty, and isf deleted what was here to match** — the worst bug
  of the lot, and it is in the pushed nightly (the agent's listings, round 4);
- ignored folders aren't watched on either side (80 of them: 4 watches
  instead of 85);
- a slow line in the tests (`ISF_TEST_BANDWIDTH_KBPS`, shared by every
  session), `t-slow`, and a bigger in-flight window for a file on its own;
- the symlink-into-a-folder fix and exclusive temp files (both were bugs);
- `--update`/`--check-update`, `--once`, the clock warning;
- the agent putting uploads in place (protocol 5), which closes the last
  window where a change made on the remote could be lost;
- fuzzing (`make fuzz`, and a CI job), the random tests (`t-chaos`,
  `t-chaos-live`) and about fifteen other new tests;
- the README cut down to what a user needs, with the rest moved here.

## Next steps

1. **Try the nightly on real machines** before calling it 0.1. So far
   everything ran against `test/fake-ssh`, on one machine. With the same
   nightly on both sides:
   - a real server over ssh, and an aarch64 one if you have one (a Pi);
   - a first sync of a real project, with `node_modules/` or `build/` in
     `.isfignore`;
   - editing with your usual editor on both sides (atomic saves, swap files);
   - renaming and deleting folders on each side, and a big folder renamed on
     the remote (renamed here, not downloaded again);
   - losing the network (wifi off, laptop suspended): it connects again and
     syncs what changed meanwhile;
   - Ctrl-C in the middle of a big first sync, then starting again;
   - `-q`, and the status line during a long first sync.
2. **Tag 0.1** once nothing surprised you (the Release workflow makes the
   release):
   ```sh
   git tag v0.1
   git push origin v0.1
   ```

## Known limitations worth fixing

- **Offline remote edits in the same second.** What changed on the remote
  while isf wasn't running is compared by size and whole-second mtime, so an
  edit that keeps the size in the same second as the last sync is missed. The
  agent now lists the remote tree at start with `lstat`: it could send the
  mtime's nanoseconds and the ctime, and remote files would be compared like
  local ones (`stamp`). A protocol change (`AGENT_PROTOCOL` 6).
- **Ignored folders are still watched**, so a huge ignored folder uses many
  inotify watches. The agent loads the remote `.isfignore` now (for its
  listings): both sides could skip watching ignored folders, and watch them
  when `.isfignore` stops ignoring them.
- **`!` patterns** aren't supported in `.isfignore`.
- **IPv6 addresses** need a host alias (`[::1]:path` isn't parsed).
- **Symlinks'** own mtimes aren't synced.

## Performance

- **A big file on a long link**, still: one file now keeps 2 MB in flight
  when it's alone in its batch (3 → 6 MB/s at 100 ms), but that is still one
  connection's worth. Splitting a big file over the `-j` connections would
  take it further.
- **Big files that change a little** are sent whole. The agent could compute
  block checksums there, rsync-style, and only the changed blocks go.
- **The local walk at start** still reads every local folder and `lstat`s
  every file: that's most of what's left of the startup time on big trees.
- **Renames while isf wasn't running** are a delete and a copy: a big folder
  renamed offline goes over again. Matching size and mtime against the record
  would find them (rsync and unison do this).
- **`LIST_MAX`** (100,000 entries): past it, the rest is listed a level at a
  time over SFTP. Raising it costs a few MB per 50,000 entries, locally.
- **The echo of a big first sync** (one `W` per file) is processed after it:
  no round trips, but 50,000 events.
- **Slow links:** document `Compression yes` for the host in
  `~/.ssh/config` (it helps text-heavy trees).

## Usability

- **Installing on the remote.** `isf --update` updates each side on its own,
  but isf could also copy itself to the remote over SFTP (to
  `~/.cache/isf/<version>/isf`) when the one there is missing or different,
  which is the first thing that goes wrong for a new user.
- **See and forget folders:** a command that lists what's remembered (the
  `dest-*` files) and forgets one, instead of deleting files by hand.
- **Running in the background:** a systemd user unit in the README, or a
  documented way to run several folders or hosts.
- **The status line during one big file with `-j 1`:** the main thread is
  inside the transfer, so it isn't redrawn until the file is done.
- **A man page and shell completions**, generated from the flags.

## Robustness and testing

- **More for the fuzzer**: `make fuzz` covers the agent's messages and SFTP
  replies. The record files and `.isfignore` are read from disk, not from the
  remote, but they could be fuzzed too.
- **A real sshd in CI**: start one on localhost with a key, and run a few
  tests through real `ssh` (ControlMaster, keepalives, a real exit status)
  instead of `fake-ssh`.
- **More seeds for the random tests in CI**: they run one seed each now. A
  nightly job could run a range and keep the ones that fail.

## Releases and packaging

- A changelog, or release notes written by hand for tagged versions. The
  Release workflow uses `--generate-notes`, which lists merged pull requests
  (there are none: you push to main) and a compare link.
- An AUR package (`isf-bin` from the release, or `isf` from source).
