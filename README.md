# isf

isf keeps a local folder and a remote one the same, both ways, over ssh.
Change a file on either side and it shows up on the other a moment later.

```
$ isf ./proj server:proj
isf: ./proj ⇄ server:proj
  ↑ src/main.c
  ↓ notes.md
isf: in sync: 1 sent, 1 received
isf: watching for changes, Ctrl-C to stop
  ↑ src/main.c
  ↓ todo deleted
```

## Requirements

- Linux on both machines (isf uses inotify to see changes).
- ssh access to the remote, with SFTP enabled (it is by default in OpenSSH).
- isf installed on both machines: the local one starts a copy on the remote
  to see the changes made there.

## Install

The [releases](https://github.com/hugoocoto/isf/releases) have isf for x86_64
and aarch64, as a static binary (`isf-x86_64`, `isf-aarch64`) and as an
AppImage. The static binary runs on any Linux of its architecture, so the same
file works on the remote too. `nightly` is built from every push to `main`.

```
curl -Lo ~/.local/bin/isf https://github.com/hugoocoto/isf/releases/download/nightly/isf-x86_64
chmod +x ~/.local/bin/isf
```

Or build it:

```
git clone --recurse-submodules https://github.com/hugoocoto/isf
cd isf
make
cp isf ~/.local/bin/
```

If you cloned without `--recurse-submodules`, run
`git submodule update --init` before `make`. `make STATIC=1` builds a static
binary, `make appimage` the AppImage, and `make test` runs the tests (they
need `sftp-server`, which comes with OpenSSH, but no ssh server).

Then copy it to the remote too:

```
scp isf server:.local/bin/isf
```

The remote copy has to be in the `PATH` of commands run over ssh, which often
doesn't include `~/.local/bin`. If it isn't, tell isf where it is with `-I`
(it's remembered, so only the first time):

```
isf ./proj server:proj -I .local/bin/isf
```

If isf can't find it, it says so and prints these commands.

## Usage

```
isf [folder...] [[user@]host:folder] [-p port] [-I path] [-j n] [-n] [-v] [--reset]
```

The first time, give the local folder and where it goes, like `scp`:

```
isf ./proj server:proj              # ./proj with ~/proj on server
isf ./proj server:/srv/proj         # an absolute path on the remote
isf ./proj me@server:proj -p 2222   # another user and port
isf ./proj server:                  # into the remote home: ~/proj
isf a b server:backups/             # several folders: backups/a and backups/b
```

After that, isf remembers where each folder syncs to:

```
isf ./proj                          # the same as last time
cd proj && isf                      # the current folder
```

The remote folder is created if it's missing. `host` can be anything `ssh`
accepts, including aliases from `~/.ssh/config`. Paths without a leading `/`
start at the remote home.

isf runs until you press Ctrl-C.

### Which remote folder

- One local folder syncs with exactly the remote folder you give.
- Several local folders, or a remote folder ending in `/` (or empty, as in
  `server:`), go inside it, each with its own name.

### Options

| Option | |
|---|---|
| `-p`, `--port PORT` | ssh port, if not the default or the one in `~/.ssh/config` |
| `-I`, `--isf PATH` | where isf is on the remote, if it isn't in the `PATH` of ssh commands there |
| `-j`, `--jobs N` | transfer up to N files at once, over N connections (default 4; 1 turns it off) |
| `-n`, `--dry-run` | show what syncing would do, change nothing, and exit |
| `-v`, `--verbose` | show every file system event, and where errors come from in the code |
| `--reset` | forget what was synced before: sync like the first time |
| `-V`, `--version` | show the version |
| `-h`, `--help` | show the help |

`-p` and `-I` are remembered with the folder, like the host.

### Speed

isf transfers several files at once, each over its own ssh connection (all
sharing one login), so a first sync of many files is much faster over a slow
link. `-j` sets how many; the default is 4. The connections are only used for
file data — everything else stays in order on one connection, so the result is
the same whatever `-j` you pick. `-j 1` transfers one at a time.

When it scans, isf lists the remote folder a level at a time, every directory
of a level in one pipelined batch, so checking a deep folder over a slow link
takes a few round trips per level rather than per directory. It opens its
connections at once too, so starting up doesn't wait for them one by one.

## What the output means

```
  ↑ path               sent to the remote: it was created or changed here
  ↓ path               received from the remote: it was created or changed there
  ↑ dir/               the same for a directory
  ↓ path deleted       removed here because it was removed there (↑: the other way)
  ↑ path mode          only the permissions changed
  ↑ old → new          renamed here, and renamed there too
  ! path changed on both sides: kept the newest, the other one is path.isf-conflict
```

With several folders, paths start with the folder they are in.

## How changes are decided

isf remembers what both sides had after the last sync. When something
changed:

- If it changed on one side only, that side wins.
- If it changed on both, the one with the newest modification time wins. If
  they are files, the other version is kept next to it as
  `FILE.isf-conflict`, on both sides, so nothing is lost.
- If one side deleted it and the other edited it, the edit wins: the file
  comes back on the side that deleted it.
- If one side deleted a directory and the other changed something inside it,
  what was changed is kept and the rest is deleted.

The first time a folder is synced there is nothing to compare with: files
that only exist on one side are copied to the other, and files that exist on
both but differ are handled as changed on both sides.

"Newest" compares the clocks of the two machines, so keep them in sync (NTP).

## Ignoring files

Put patterns in a `.isfignore` file in the synced folder, one per line, like
`.gitignore`:

```
# any name that matches, in any directory
*.log
# a trailing / matches only directories
build/
# a leading / (or one in the middle) is relative to the folder
/secret.txt
docs/*.tmp
```

Lines starting with `#` are comments; a `#` after a pattern is part of it.
`*`, `?` and `[...]` work as in the shell. Patterns starting with `!` aren't
supported.

Editor temp files are always ignored: `*.swp`, `*.swx`, `*~`, `4913` and
`.#*`.

`.isfignore` is synced like any other file, so both sides use the same
patterns. Changes to it apply right away: isf looks at the whole folder again
with the new patterns, so what they don't ignore anymore is synced. Files that
were synced before a pattern was added stay where they are, but aren't synced
anymore.

## Safety

- Files are written to a temporary `.isf.<pid>.<n>.tmp` next to them and renamed
  into place, so a half-written file is never seen.
- Only one isf can sync a given folder to a given place at a time; a second
  one exits with a message instead of racing the first.
- If a folder that was synced before is empty on one side when isf starts,
  isf stops instead of deleting everything on the other side. This is what
  you'd see after pointing it to the wrong place or losing a disk. If that
  side was emptied by mistake, `isf --reset` copies everything back.
- If the remote folder is removed or moved while isf runs, isf stops.
- isf decides what to do for a whole folder, then does it. Before it replaces
  or removes a local file, it checks that the file is still what it decided
  on; if you changed it meanwhile, it's left alone, and your change is synced
  next.
- A remote folder that can't be read (permissions) is skipped with an error,
  not taken for an empty one.
- `isf -n` shows what a sync would do without doing it.

## Where isf keeps its things

- `~/.local/state/isf/` (or `$XDG_STATE_HOME/isf/`): what was last synced
  for each folder, and where each folder syncs to (the `dest-*` files).
  `--reset` deletes the first; delete a `dest-*` file to make isf forget
  where a folder goes.
- `$XDG_RUNTIME_DIR/isf-*` (or `~/.ssh/isf-*`): the ssh connection socket
  while isf runs. isf opens several ssh sessions (metadata, the remote copy of
  isf, and one per parallel transfer) that all share one connection, so you log
  in once.

## Small machines

isf asks little of the remote. Syncing 50,000 files, it runs there an agent
that uses about 2 MB, and `sftp-server` processes (one, plus one per `-j`
connection) of 2 to 5 MB each. The local isf uses more (about 30 MB for those
50,000 files), so the bigger machine should be the local one.

What can run out on a small remote:

- **inotify watches.** The agent watches every folder, and Linux limits how
  many watches a user has: on a machine with 1 GB of memory, about 8,000. Each
  one also keeps about 1 KB of kernel memory. If the limit is reached, isf says
  so and how to raise it; until then, changes in the folders it couldn't watch
  are only seen when isf starts.
- **CPU for ssh.** All of isf's sessions share one ssh connection, so one
  process on each side encrypts everything. On a slow CPU that bounds the
  transfer speed, whatever `-j` is; `-j` helps with many small files, where
  the round trips add up. On ARM boards without AES instructions,
  `Ciphers chacha20-poly1305@openssh.com` for that host in `~/.ssh/config` is
  faster.
- **Disk.** Parallel transfers compete for a slow disk (an SD card); `-j 2`
  can be faster there.

## Limitations

- Changes made on the remote are compared by size and modification time, in
  whole seconds. An edit there that keeps the size, in the same second as the
  last sync, can be missed. Local changes don't have this problem.
- A rename on the remote is copied as a delete and a new file, so a renamed
  big directory is downloaded again. Renames made locally are renamed on the
  remote.
- Symlinks are synced, but not their own modification times.
- Ignored directories that exist when isf starts are still watched, only
  their events are dropped. A huge ignored directory uses many inotify
  watches.
- If the connection drops, isf stops. Run it again: it syncs what changed
  meanwhile.
- isf runs in the foreground, one host at a time.
- IPv6 addresses need a host alias in `~/.ssh/config`.
