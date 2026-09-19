# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

isf is a two-way folder sync over ssh/SFTP, written in C for Linux (it uses inotify on both machines). `README.md` is the user documentation: CLI, output symbols, how conflicts are decided, `.isfignore`, state files, small machines. `NEXT-SESSION.md` holds handoff notes between sessions; it isn't part of the build.

Never run `git add`, `git commit` or `git push`: show Hugo the commands instead.

## Build and test

```
git submodule update --init   # thirdparty/{cum.h,flag.h,bt.h} are submodules
make                          # ./isf (-O2 by default: CFLAGS=... replaces it)
make test                     # test/unit, then every test/t-*.sh
test/run.sh t-conflict        # one integration test (or several)
make STATIC=1                 # static binary, what the releases ship
make appimage                 # isf-ARCH.AppImage (appimage.sh; fetches appimagetool into .cache/)
```

The Makefile appends to `CFLAGS`, so extra flags go through the environment, and changing only flags needs `make -B`. The tests run against any binary via `ISF=`. Build sanitizer binaries under another name so `./isf` stays normal:

```
CFLAGS="-g -O1 -fsanitize=address,undefined" make -B OUT=/tmp/isf-asan
ASAN_OPTIONS=detect_leaks=0 ISF=/tmp/isf-asan test/run.sh
CFLAGS="-g -O1 -fsanitize=thread" make -B OUT=/tmp/isf-tsan && ISF=/tmp/isf-tsan test/run.sh
```

A sanitizer report in isf's stderr fails the test (`check_sanitizers` in `test/lib.sh`).

How the tests work:
- The "remote" is this machine. `test/fake-ssh` stands in for `ssh`, which isf finds through `PATH`. It runs `sftp-server` for `-s sftp` and `sh -c` for the agent, from `$ISF_TEST_REMOTE`.
- `test/lib.sh` gives each test `$L` (local folder) and `$R` (remote folder), with `start`/`stop`/`run`, `wait_same` (compares `tree` listings) and `expect_*`.
- `XDG_STATE_HOME` points at the test's temp dir.
- Only `sftp-server` is needed, no ssh server.
- Variables `fake-ssh` reads:
  - `ISF_TEST_LATENCY_MS`: round-trip delay on SFTP traffic, through `test/latency.py`.
  - `ISF_TEST_SFTP_LIMIT`: refuse SFTP sessions after that many.
- `t-watch-limit.sh` re-runs itself under `unshare -Ur` to lower the inotify limit.
- `test/bench.sh SHAPE FILES [RTT_MS]` times a first sync and an in-sync restart.
- `test/mem.sh FILES DIRS SIDE` prints each process's peak memory.
- CI (`.github/workflows/release.yml`) runs `make test`, then publishes the static binary, the AppImage and the source for x86_64 and aarch64. The `nightly` release follows `main`; `v*` tags get releases of their own.

## Architecture

The one binary plays two roles:

- **Local (`main.c`).** Parses the scp-style CLI, remembers destinations (`dest-<hash>` files), watches local folders, and runs the event loop.
- **Remote agent (`agent.c`, hidden `isf --agent DIR...`).** Started over ssh. It watches the remote folders with inotify and reports changes on stdout: a type byte (`R`/`C`/`D`/`O`), the root index byte, then the path ending in NUL. It never transfers data.

All ssh sessions share one ControlMaster connection:
- the main `Sftp` (`sftp.c`, a hand-written SFTP v3 client; no libssh);
- the agent;
- `-j` transfer connections in `pool.c`.

One `Sftp` belongs to one thread.

**Event loop (`main.c`).** Events call `mark()`, which adds to a per-root dirty set (a bt.h tree: rel → SYNC_* bits). After 100 ms of quiet, or 1 s at most, `flush()` walks each tree in order. It skips paths under a directory already handled whole, calls `reconcile()` for the rest, then `sync_drain()`, then saves the records. A changed `.isfignore` is synced first, then its root is rescanned with the new patterns. The startup order matters: connect, `sync_open` (lock, record, ignore, `looks_wiped`), agent ready, local watches, then the first flush.

**Deciding and doing (`plan.c`, `sync.c`).** `reconcile()` builds a `Walk`:
- **The walk.** `plan_path`/`plan_dir`/`plan_children` read the local state, the remote state (from listings) and the record, and append `Action`s. The pure decisions live in `plan.c` and are unit-tested:
  - `plan_file`: whichever side changed wins; if both changed, the newer mtime; an edit beats a deletion.
  - `decide_dir` and `dir_mode`.
- **Applying.** `apply_ready()` carries out the steps as soon as they're planned, so transfers overlap the rest of the walk.
  - A directory's closing steps (RMDIR, or MODE+RECORD) are planned after its contents, from what was planned for them (`plan_keeps`).
  - A failed step skips later steps for the same path and everything under it.
  - Steps that change the local side first check it still matches what was planned (`local_unchanged`). A download checks again just before its final rename.
  - Regular-file COPYs go to the pool. They're recorded and reported in `transfer_done` during `sync_drain`, which also sets the directory modes that were held back.
  - `--dry-run` goes through the same path; `g.dry_run` makes each step report instead of act.

**Remote listings (`sync.c`, "read ahead").** When a walk first needs a directory's listing, `read_ahead` lists the whole remote tree under it level by level, one `sftp_readdir_many` batch per level, capped at `READAHEAD_MAX` entries. Results go in a bt.h map that `take_listing` consumes. If a path was never listed but its parent was, it's known to be missing (empty listing, no round trip). A failed listing is an error, not an empty directory. `sftp_readdir_many` splits each level into rounds of 128 directories, which bounds the memory `sftp-server` uses on the remote to queue replies.

**Records.** Each root keeps a `Da(Record)` sorted by rel. Everything under `rel/` is one contiguous range, `["rel/", "rel0")`, which `record_range` finds by binary search. Deletes, child listings and renames are range operations; don't go back to linear scans (the old version was O(n²)). Records are saved in the state directory under an FNV-1a hash of host, port, remote and local path. A `.lock` sidecar is held with `flock`.

## Invariants

- A path from the remote (agent message or listing) must pass `path_safe`/`name_safe` before it builds a local path.
- Transfers write `.isf.<pid>.<worker>.tmp` and rename it into place. `is_temp_name` names are never synced, and both inotify handlers skip them.
- `SFTP_ERR_IO` on the main connection is fatal: `sftp_ok` exits. Worker connections set `Sftp.dead`, and `flush()` exits after the drain.
- bt.h has one global iterator (`bt_iter`/`for_bt_each`). `main.c` uses it on the dirty set while it calls `reconcile()`, so code reached from `reconcile` must never iterate a bt.h tree. That's why the listing cache also keeps its entries in a plain `Da`.
- `Da_insert(da, e, i)` evaluates `i` after it has appended a zeroed element, so compute the index first.
- User-visible output (`↑ ↓ !` lines), flags and behavior are documented in `README.md` and in `main.c`'s `HELP`/`flag_add` strings. Keep them in sync.

## Conventions

- C (gnu17) with 8-space indents. Function definitions put the return type on its own line. `=` aligns across consecutive declarations.
- Comments are plain English and name parameters in CAPS. Header comments are the API docs.
- Use `cum.h` dynamic arrays (`Da(T)`, `Da_append`, `Da_foreach`, `Da_destroy`). `Da(T)` is an anonymous struct, so typedef it before passing it around. Build argv with `Command_add`.
- `flag.h` flags are `const char *`, freed by `flag_free()`.
- Log with the `util.h` macros: `LOG_WARN`, `LOG_ERR` (adds `strerror(errno)`), `LOG("Error", …)`, and `VPRINT` for verbose-only output.
- Module state lives in a file-level `static struct { … } g;`.
