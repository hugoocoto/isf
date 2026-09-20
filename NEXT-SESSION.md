# isf — what's left, and ideas

Notes to resume from. (This file isn't part of the build; delete it whenever.)
As always: don't commit, `git add` or `git push` anything; show the commands.

Round 4 is pushed (8831fa8), with the release-date fix after it (758f0cd),
both green on CI. What came after that isn't pushed yet:
- the symlink-into-a-folder fix and exclusive temp files (both were bugs);
- `--update`/`--check-update`, `--once`, the clock warning;
- the agent putting uploads in place (protocol 5), which closes the last
  window where a change made on the remote could be lost;
- fuzzing (`make fuzz`, and a CI job), the random tests (`t-chaos`,
  `t-chaos-live`) and about fifteen other new tests;
- the README cut down to what a user needs, with the rest moved here.

## Next steps

0. **Check how isf behaves on a slow link** (Hugo asked for this next). The
   tests run with `ISF_TEST_LATENCY_MS` and `test/bench.sh` takes an RTT, so
   start there: 200–1000 ms round trips, and a link that is slow as well as
   late (there is no bandwidth limit in `test/latency.py` yet — adding one is
   part of the job). What to look at:
   - does a first sync of a real-sized tree finish, and how long does it take;
   - the status line while it goes (it should say something within 2 s);
   - a big file: 512 KB in flight per connection caps it at about 5 MB/s at
     100 ms (see Performance below) — measure it, and see whether `-j` helps;
   - changes made on both sides while transfers are still going (`t-chaos-live
     LAT=200`), and that both sides end the same;
   - reconnects: `t-reconnect` with latency, and a link that drops mid-transfer;
   - the debounce (100 ms) and `MAX_DELAY_MS` (1 s) against a link where a
     round trip is longer than both: does isf pile up flushes?
   - the agent's placement requests (protocol 5) add a round trip per batch
     of 128 at the end of a flush: check it doesn't dominate a slow link.

1. **A known bug, found by the random test** (not yet fixed): with
   `SEED=59 ROUNDS=8 test/run.sh t-chaos`, round 7 leaves a folder with mode
   755 here and 644 there. It comes from a folder renamed here while it was
   chmod'ed there (an offline rename, so isf sends it as a delete and a new
   folder). isf reports `↓ dir/x.moved/ mode` in that run but the modes stay
   apart; a second run puts them right, so it's the mode the first run picks,
   not a missing pass. Nothing is lost, and only the mode is wrong.

2. **Push the release workflow change** (`.github/workflows/release.yml`): the
   nightly release is deleted and made again on every push, so its date is
   the build's. Check on the next push that the date changes and the download
   links still work.
3. **Try the nightly on real machines** before calling it 0.1. So far
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
4. **Tag 0.1** once nothing surprised you (the Release workflow makes the
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
  local ones (`stamp`). A protocol change (and `AGENT_PROTOCOL` 4).
- **Ignored folders are still watched**, so a huge ignored folder uses many
  inotify watches. The agent loads the remote `.isfignore` now (for its
  listings): both sides could skip watching ignored folders, and watch them
  when `.isfignore` stops ignoring them.
- **`!` patterns** aren't supported in `.isfignore`.
- **IPv6 addresses** need a host alias (`[::1]:path` isn't parsed).
- **Symlinks'** own mtimes aren't synced.

## Performance

- **A big file on a long link.** A connection has 512 KB in flight
  (`MANY_DATA`: 16 × 32 KB), so one file goes at most 512 KB per round trip:
  about 5 MB/s at 100 ms. OpenSSH's `sftp` keeps 64 × 32 KB. The window
  could grow while a connection has few files (it's there to bound
  `sftp-server`'s memory with many small ones), or a big file could be split
  over the `-j` connections.
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
- **`--once`:** sync and exit, for scripts and cron (like `-n`, but doing it).
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
- **A test of `LIST_MAX`**: the fallback past it is the same code as for an
  unreadable folder (tested), but the limit itself isn't reached by any test.

## Releases and packaging

- A changelog, or release notes written by hand for tagged versions. The
  Release workflow uses `--generate-notes`, which lists merged pull requests
  (there are none: you push to main) and a compare link.
- An AUR package (`isf-bin` from the release, or `isf` from source).
