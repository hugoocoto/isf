# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

isf is a two-way folder sync over ssh/SFTP, written in C for Linux (it uses inotify on both machines). `NEXT-SESSION.md` holds handoff notes between sessions; it isn't part of the build.

`README.md` is for people who use isf, and Hugo wants it short: what it is, how to install it, the commands and flags, `.isfignore`, what the output means, and the limitations that bite. No internals, no numbers about round trips, no design notes — those go here. When behavior changes, check whether the README needs a line, not a paragraph.

## What it costs (for the README's "small machines" line, and for judging changes)

Syncing 50,000 files: the agent on the remote uses about 2 MB, each `sftp-server` 2–5 MB (one, plus one per `-j` connection), and the local isf about 30 MB. Over a 50 ms link: 2000 small files in about 2 s, an in-sync start in under half a second whatever the depth, a first sync of a deep tree about 1 s. The remote's inotify watches are the limit on small machines (about 8,000 with 1 GB of memory, 1 KB of kernel memory each); isf says so and how to raise it. On ARM boards without AES instructions, `Ciphers chacha20-poly1305@openssh.com` in `~/.ssh/config` is faster; parallel transfers compete for a slow disk, so `-j 2` can beat `-j 4` there.

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
- What `fake-ssh` reads:
  - `ISF_TEST_LATENCY_MS`: round-trip delay on SFTP and agent traffic, through `test/latency.py` (which exits with its program's status).
  - `ISF_TEST_SFTP_LOG`: count the SFTP requests (`latency.py` writes them per connection, with the relay's `start` time). In a test, `count_requests` turns it on; then `requests TYPE` sums a type (`MKDIR`, `READ`, `rounds`...) and `main_requests TYPE` reads the main connection only: the first one opened since `requests_clear`.
    - A "round" is a request sent after a reply came back: pipelined requests make one, one-by-one requests one each. Use it to guard round trips.
    - Each relay keeps its totals until it's killed, so `requests_clear` copies them as a baseline (`sftp-base.*`) that the counts subtract.
  - `ISF_TEST_SFTP_FLAGS`: passed to `sftp-server`. `-P posix-rename` makes a server without it (`t-no-posix-rename`).
  - `ISF_TEST_SFTP_LIMIT`: refuse SFTP sessions after that many.
  - A file named `down` next to the fake remote home: the host is unreachable (exit 255). `t-reconnect.sh` uses it.
- `WAIT` (seconds, default 10) is how long `start`/`wait_same`/`wait_for` wait. It's measured on a clock: raise it for tests with latency.
- Timing-sensitive tests trigger on requests, not sleeps. For example, `t-remove-race` edits once the removals are counted.
- `t-progress` runs isf under `script` to give it a terminal (the status line is off on a pipe).
- `t-watch-limit.sh` re-runs itself under `unshare -Ur` to lower the inotify limit.
- `t-chaos` and `t-chaos-live` make random changes on both sides (`SEED=`, `ROUNDS=`, `LAT=` to repeat one): after each round the two sides have to be identical, with `--once` and while watching. They found the `--once` gap above; run them with a few seeds after changing how syncing decides things.
- Build with `-DLIST_MAX=0` and the whole suite runs on the SFTP listing path instead of the agent's (only `t-requests`, which counts them, fails).
- `test/bench.sh SHAPE FILES [RTT_MS] [SIDE]` times three things: a first sync, a change made right after it (the cost of isf's own echoes), and an in-sync restart.
- `test/mem.sh FILES DIRS SIDE` prints each process's peak memory.
- `make fuzz CC=clang` builds `test/fuzz` (libFuzzer, ASan+UBSan): what the other side sends, the agent's messages and SFTP replies. The first byte of an input picks which. `test/fuzz -max_total_time=60`.
- CI:
  - `.github/workflows/test.yml` runs `make test` on every push and pull request: gcc, clang, ASan+UBSan and TSan.
  - `.github/workflows/release.yml` publishes the static binary, the AppImage and the source for x86_64 and aarch64. The `nightly` release follows `main`; `v*` tags get releases of their own.

## Architecture

The one binary plays two roles:

- **Local (`main.c`).** Parses the scp-style CLI, remembers destinations (`dest-<hash>` files), watches local folders, and runs the event loop.
- **Updating itself (`update.c`).** `--check-update` and `--update` ask GitHub for the newest release (the nightly's tag for builds from main, the newest tag otherwise), and `--update` downloads the file for this build (architecture at compile time, AppImage from `$APPIMAGE`) next to the one it runs from and renames it over it. It downloads with `curl` or `wget`, checks what came down is an ELF that runs and says `isf `, and leaves the old one alone otherwise. Exit status: 0 newest, 1 a newer one exists (`--check-update`), 2 it couldn't be done.
- **Remote agent (`agent.c`, hidden `isf --agent DIR...`).** Started over ssh. It watches the remote folders with inotify and reports changes on stdout. It never transfers data.
  - Its ready message carries `AGENT_PROTOCOL` and its version. The local side refuses anything but the same version (a protocol number can mean something else in another build), including protocol 1 agents from before versions.
  - Before it, `T` carries the time there: the local side warns if the clocks are more than 5 s apart, since they decide which side changed a file last.
  - It makes the remote folders if they're missing, watches them, sends their listings (`L`), then says it's ready.
  - Each change carries the path's lstat (size, mtime, mode) and a kind: `C` a write by anyone but isf, `W` isf's own upload (a rename from its temp file, paired by inotify cookie), `A` attributes only, `D` a directory, `M` a rename inside the folder (MOVED_FROM paired with its MOVED_TO; one left unpaired for 50 ms is sent as the `C`/`D` it is).
  - The message format is in `agent.h`. Change it only together with `AGENT_PROTOCOL` (5 now; 1, 3 and 4 went out in nightlies).
  - **It also puts files in place** (`P` request, `p` answer): isf writes the file to a temp file over SFTP, then asks the agent to lstat the target and rename over it, both there, so a change made in between isn't lost (over SFTP there is a round trip between the two). The agent refuses if the target isn't what isf expected (`c`, and it drops the temp file), and leaves anything unusual (a directory in the way) to isf (`e`, temp file kept), which then does it over SFTP as before (`place_over_sftp`). It only accepts paths inside its folders, with a temp name as the source.
  - `L` listings: each folder once it's watched, in messages of up to 64 KB, not the folders the remote's `.isfignore` ignores, and no more after `LIST_MAX` entries. A folder it didn't list is listed over SFTP.
- **Files held open** (`held_*` in `watch.c`, both sides). An IN_MODIFY without its IN_CLOSE_WRITE (a log) is synced once the writes stop for 2 s, or every 30 s while they don't.

All ssh sessions share one ControlMaster connection, with keepalives:
- the main `Sftp` (`sftp.c`, a hand-written SFTP v3 client; no libssh);
- the agent;
- `-j` transfer connections in `pool.c`.

One `Sftp` belongs to one thread. Once a connection is dead, every call on it fails at once.

**Losing the connection.** A broken connection doesn't exit. `sftp_ok` notes it once (`sync_lost()`), and the rest of the walk fails fast without changing anything. `watch_loop` then calls `reconnect()`: back off, reconnect, restart the agent, rerun `sync_check` (the wiped-side check), and rescan everything. An agent that exits 1 stopped on purpose (its folder is gone), so isf stops.

**Errors.** `LOG("Error", …)` counts into `log_errors` (atomically: workers log too). The first sync's summary says "not all in sync …, N errors" when there were any.

**Event loop (`main.c`).** Events call `mark()`, which adds to a per-root dirty set (a bt.h tree: rel → SYNC_* bits). After 100 ms of quiet, or 1 s at most, `flush()` walks each tree in order. It skips paths under a directory already handled whole, calls `reconcile()` for the rest, then `sync_drain()`, then saves the records.
- `rescan(root)` marks `.isfignore` and the whole root. A marked `.isfignore` is synced first, then the root is rescanned with its patterns. After a flush, an `.isfignore` whose ctime changed (`g.ign_stamp`, e.g. received from the remote) is loaded and the root rescanned.
- A flush runs again (up to 4 rounds) while isf's own steps left something for the other side: a file kept aside as a conflict copy is new to it. `sync_take_extra` hands those paths over (`kept_aside` records them); waiting for the events they raise instead would leave `--once` with the two sides apart, which `t-chaos` caught.
- After isf puts a file in place on the remote, `seen_now` records what is there: messages the agent sent earlier about isf's own doing (its conflict-copy rename arrives as `M`) would otherwise leave the plan believing the path is gone, and the next round of the same flush would delete the local copy. `t-chaos` caught that too.
- The startup order matters: connect; `sync_open` (lock, record, ignore); the agent, ready, with its listings (`root->seed`); `sync_check` (the wiped-side check, which lists the root over SFTP only if the agent didn't); local watches; `rescan`; the first flush. The listings are a snapshot taken after the watches: what changes later arrives as events. Reconnecting forgets old listings (`sync_forget_seed`) and does the same again. The pool connects in the background (`pool_settle` waits for it the first time it's needed).
- **Output.** Everything printed goes through `say()` (the LOG and VPRINT macros too), so the status line (`status_line()` in `util.c`, stdout on a terminal only) is cleared first and redrawn under it. `sync_progress(1/0)` brackets a flush; after 2 s, `progress_tick` (called from the walk and while `pool_drain` waits) shows the folders listed, then files and bytes (`sftp_moved`, counted by the transfer engine) of those planned. `-q` drops the ↑/↓ lines only.

**Echoes.** isf's own writes come back as events, and each would cost a round trip. So:
- Agent events store what the agent saw in `root->seen`, and `plan_path` uses that as the remote state instead of an LSTAT (symlinks still LSTAT, for their target).
- An `M` renames the local copy (`sync_remote_rename`) when both paths are what was recorded and the new one is free here; otherwise both paths are marked (a delete and a fetch). Its own echo here is dropped by `moved()`.
- A `C` event sets `written` on the path: a write by someone else. `plan_file` treats a written file as changed there, whatever its size and mtime say. That catches same-second, same-size edits, and edits racing isf's own upload.
- isf's own remote actions besides uploads also produce `C` events, so they're registered first as expectations (`expect()`, `root->expect`):
  - its renames (`sync_rename`);
  - conflict copies (`keep_remote_copy`);
  - uploads put in place by hand: a server without posix-rename renames with link+unlink, and removes the old target first.
  - A `C` matching an expectation's state is consumed instead of setting `written`. Up to 2 expected states per path; forgotten after a minute.
- Local events go through `mark_local`, which drops a file that still matches its record (`sync_unchanged`).
- A new directory's event is skipped only if isf made it: the `sync_on_local_dir` hook watches it at once and adds it to `g.made`. Any other directory that appears is rescanned whole, since its contents have no events of their own.

**Deciding and doing (`plan.c`, `sync.c`).** `reconcile()` builds a `Walk`:
- **The walk.** `plan_path`/`plan_dir`/`plan_children` read the local state, the remote state (from listings) and the record, and append `Action`s. The pure decisions live in `plan.c` and are unit-tested:
  - `plan_file`: whichever side changed wins; if both changed, the newer mtime; an edit beats a deletion.
  - `decide_dir` and `dir_mode`.
- **Applying.** `apply_ready()` hands steps to a batch as they're planned, so transfers overlap the rest of the walk. `batch_flush` does a batch:
  - It flushes at 256 steps, at the walk's end, and before a MKDIR with something in the way, since the MKDIRs inside it need that one done first.
  - A remote file to remove is first moved aside to a temp name and checked there (RENAME+LSTAT, one round). One `sftp_batch` round then sends every MKDIR, REMOVE of what was moved aside (or RENAME back, if it changed), and RMDIR, in plan order. The server does requests in order, so parents come before children and contents before their RMDIR. A write there after the move makes a new file, which stays.
  - A failed MKDIR is retried alone with `sftp_mkdir_p` (already there, or a parent missing).
  - Then, in order: the results, and the steps that waited (records, local steps, transfers). Transfers go only now, when their folder exists.
  - Dry runs don't batch.
  - A directory's closing steps (RMDIR, or MODE+RECORD) are planned after its contents, from what was planned for them (`plan_keeps`, from the counts in `Walk.keeps`).
  - `apply_ready` drops the steps done (up to the first still batched), so `Walk.plan` holds the steps not done yet, not the whole tree's. Indices into it don't survive an `apply_ready`.
  - A failed step skips later steps for the same path and everything under it.
  - Steps that change the local side first check it still matches what was planned (`local_unchanged`). A download checks again just before its final rename.
  - Steps that replace something remote check it with an LSTAT (`remote_unchanged`). An upload checks just before its rename, inside the engine.
  - A remote change can still land in the round trip between that check and the upload's RENAME. SFTP has no compare-and-swap. The README says so; tests must not assume otherwise. (Removals don't have this window: they move the file aside first.)
  - Temp files older than a day (`TEMP_MAX_AGE`) found on either side are leftovers of interrupted transfers: `plan_leftover` plans quiet REMOVEs.
  - A download's missing local parents are made on the main thread (`local_parents`), with the watch hook.
  - An upload's `SftpFile.target` is NULL when the agent will place it: the engine writes the temp file and stops. `transfer_done` holds those back, and `sync_drain` has them placed (`place_pending`), in chunks of 128 requests so neither side's pipe fills while the other waits. Reporting and recording happen after placement.
  - Regular-file COPYs become `Transfer`s (`transfer_open`/`transfer_run`/`transfer_finish`): to the pool, or run inline as a batch of one. They're recorded and reported in `transfer_done`, from `sync_drain` for pooled ones. `sync_drain` also sets the directory modes that were held back, the remote ones in one `sftp_batch`.
  - `--dry-run` goes through the same path; `g.dry_run` makes each step report instead of act.

**Remote listings (`sync.c`, "read ahead").** When a walk first needs a directory's listing, `read_ahead` lists the whole remote tree under it level by level, one `sftp_readdir_many` batch per level, capped at `READAHEAD_MAX` entries. Results go in a bt.h map (`ahead`) that `take_listing` consumes. If a path was never listed but its parent was, it's known to be missing: an empty listing with no round trip. It's then remembered as listed, so its own children are known missing too. A failed listing is an error, not an empty directory. `sftp_readdir_many` splits each level into rounds of 128 directories, which bounds the memory `sftp-server` uses on the remote to queue replies.
- The first walk of a whole root starts from `root->seed`: the root's listing (from the agent, or `looks_wiped`) and, from the agent, the folders below (`seed.below`). `seed_below` puts those in `ahead` and adds a placeholder (not listed) for each subfolder the agent didn't list, so it's listed over SFTP rather than taken for missing. Keep that invariant: a listed folder's subfolders are all in `ahead`, listed or not, or they look empty and their contents deleted. `queue_subdirs` skips folders already listed.
- The `.isfignore` pre-step reads direct children of the root from the seed. If it sends something, the seed is forgotten (`sync_forget_seed`): the remote isn't what was listed anymore.

**Transfers (`sftp.c`, `sftp_put_many`/`sftp_get_many`).** A batch of files goes through one request queue, with every file's requests in flight together. Replies come back in order, so the queue says what each one is for.
- Caps: 256 requests, and `MANY_DATA` bytes of WRITE/READ in flight (16 × 32 KB), to keep `sftp-server`'s memory small. Many small files fit together; a big one streams 32 KB requests.
- A put: OPEN tmp, WRITEs, FSETSTAT, CLOSE and an LSTAT of the target all together, then posix-rename if the target still matches `expect`.
- A get reads exactly `size` bytes. A file that turns out shorter reports `SFTP_CHANGED`.
- Answers that don't matter (a get's CLOSE, a failed put's REMOVE) go to `s->pending`, like `close_handle_async`.
- Pool workers take up to `POOL_BATCH` (64) jobs at once. `pool_drain` hands jobs back as they finish (so their lines show as they go), and calls a tick while it waits.
- A `Transfer` waiting in the queue stays small: what's only needed while it's under way (the local file's stat) lives in `transfer_run`'s arrays.
- Temp names come from a process-wide counter (`temp_path`).
- `sftp_batch` sends any list of LSTAT/SETSTAT/MKDIR/RMDIR/REMOVE/RENAME (plain: it won't replace) together and reads every reply, with a status and error text for each.
- `transfer_finish` keeps the failing step's error text (`why`) before cleanup requests overwrite `c->error`.

**Records.** Each root keeps a `Da(Record)` sorted by rel. Everything under `rel/` is one contiguous range, `["rel/", "rel0")`, which `record_range` finds by binary search. Deletes, child listings and renames are range operations; don't go back to linear scans (the old version was O(n²)). Records are saved in the state directory under an FNV-1a hash of host, port, remote and local path; the format is described above `record_load`. Records with a 6th number, from a build between releases, still load. A `.lock` sidecar is held with `flock`.

## Invariants

- A path from the remote (agent message or listing) must pass `path_safe`/`name_safe` before it builds a local path.
- Transfers write `.isf.<pid>.<n>.tmp` and rename it into place. `is_temp_name` names are never synced. The local inotify handler skips them. The agent skips them too, except to pair their renames (`W`).
- A temp file is always made, never opened: `temp_create` (local, `O_EXCL`) and `FXF_EXCL` (remote), with another name if it's taken. Something left in its place could be a symlink to anywhere, and the transfer would write through it.
- Nothing inside the synced tree is read or written through a symlink. A directory that replaces one is made before what's inside it is planned (`plan_dir`), or the old symlink's target would be read as ours.
- After `SFTP_ERR_IO`, nothing exits: everything left in the walk fails fast (`s->dead`), and `watch_loop` reconnects.
- Don't add to or delete from a bt.h tree while walking it (`for_bt_each`): that moves entries between nodes. Walks themselves nest fine; avoid the old `bt_iter`, which has one global state. bt.h's own tests (in the submodule: `make && ./test`) check the red-black invariants after every operation.
- `Da_insert(da, e, i)` evaluates `i` after it has appended a zeroed element, so compute the index first.
- User-visible output (`↑ ↓ !` lines), flags and behavior are documented in `README.md` and in `main.c`'s `HELP`/`flag_add` strings. Keep them in sync.

## Conventions

- C (gnu17) with 8-space indents. Function definitions put the return type on its own line. `=` aligns across consecutive declarations.
- Comments are plain English and name parameters in CAPS. Header comments are the API docs.
- Use `cum.h` dynamic arrays (`Da(T)`, `Da_append`, `Da_foreach`, `Da_destroy`). `Da(T)` is an anonymous struct, so typedef it before passing it around. Build argv with `Command_add`.
- `flag.h` flags are `const char *`, freed by `flag_free()`.
- Log with the `util.h` macros: `LOG_WARN`, `LOG_ERR` (adds `strerror(errno)`), `LOG("Error", …)`, and `VPRINT` for verbose-only output. Other output during a flush goes through `say()`, not `printf`, so it doesn't write over the status line.
- Module state lives in a file-level `static struct { … } g;`.
