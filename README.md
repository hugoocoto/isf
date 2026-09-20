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
- The same isf on both machines: the local one starts a copy on the remote to
  see the changes made there, and stops if it isn't the same version.
- `curl` or `wget`, only for `isf --update`.

## Install

The [releases](https://github.com/hugoocoto/isf/releases) have isf for x86_64
and aarch64, as a static binary (`isf-x86_64`, `isf-aarch64`) and as an
AppImage. `nightly` is built from every push to `main`.

```
curl -Lo ~/.local/bin/isf https://github.com/hugoocoto/isf/releases/download/nightly/isf-x86_64
chmod +x ~/.local/bin/isf
```

Or build it:

```
git clone --recurse-submodules https://github.com/hugoocoto/isf
cd isf
make && cp isf ~/.local/bin/
```

Then put the same isf on the remote, for example with
`scp isf server:.local/bin/isf`. If it isn't in the `PATH` of commands run
over ssh (`~/.local/bin` often isn't), say where it is once and isf remembers:

```
isf ./proj server:proj -I .local/bin/isf
```

`isf --update` replaces isf with the newest release; run it on both machines.
`isf --check-update` only says whether there is a newer one.

## Usage

```
isf [folder...] [[user@]host:folder] [options]
```

The first time, give the local folder and where it goes, like `scp`:

```
isf ./proj server:proj              # ./proj with ~/proj on server
isf ./proj server:/srv/proj         # an absolute path on the remote
isf ./proj me@server:proj -p 2222   # another user and port
isf a b server:backups/             # several folders: backups/a and backups/b
```

A remote folder ending in `/` (or `server:` by itself) means "inside it",
each folder keeping its name; it's created if it's missing. `server` can be
anything ssh accepts, including an alias from `~/.ssh/config`.

After that, isf remembers where each folder syncs to:

```
isf ./proj                          # the same as last time
cd proj && isf                      # the current folder
```

isf runs until you press Ctrl-C. With `--once` it syncs what's different now
and exits instead, for a script or a cron job.

### Options

| Option | |
|---|---|
| `-p`, `--port PORT` | ssh port, if not the default or the one in `~/.ssh/config` |
| `-I`, `--isf PATH` | where isf is on the remote, if it isn't in the `PATH` of ssh commands there |
| `-j`, `--jobs N` | transfer up to N files at once (default 4) |
| `-n`, `--dry-run` | show what syncing would do, change nothing, and exit |
| `--once` | sync what's different now and exit |
| `-q`, `--quiet` | don't list each file sent or received |
| `-v`, `--verbose` | show every file system event |
| `--reset` | forget what was synced before: sync like the first time |
| `--check-update` | say whether a newer isf has been released |
| `--update` | replace this isf with the newest release |
| `-V`, `--version` | show the version |
| `-h`, `--help` | show the help |

`-p` and `-I` are remembered with the folder, like the host.

## What the output means

```
  ↑ path               sent to the remote (↓: received from it)
  ↑ dir/               the same for a directory
  ↑ path deleted       removed there because it was removed here
  ↑ path mode          only the permissions changed
  ↑ old → new          renamed on both sides
  ! path changed on both sides: kept the newest, the other one is path.isf-conflict
```

With several folders, paths start with the folder they are in. A sync that
takes more than a couple of seconds also shows how far it got, on a line that
stays at the bottom.

## What it syncs

Files, folders and symlinks — symlinks as symlinks, without following what
they point at — with their permissions and modification times. Not owners or
groups, and not hard links: two names for one file become two files on the
other side. Anything else (sockets, fifos, devices) is skipped with an error.

## How changes are decided

- If something changed on one side only, that side wins.
- If it changed on both, the newest modification time wins, and the other
  version is kept next to it as `FILE.isf-conflict` on both sides.
- If one side deleted it and the other edited it, the edit wins.
- A file that is kept open while it's written (a log) is synced once the
  writes stop for a couple of seconds, not at every write.
- The first time a folder is synced, files that exist on only one side are
  copied, and files that differ on both are treated as changed on both.

"Newest" compares the clocks of the two machines, so keep them in time (NTP).
isf says so at the start if they disagree.

## Ignoring files

Put patterns in a `.isfignore` file at the top of the synced folder, one per
line, like `.gitignore` (only that one is read, not any deeper down):

```
# any name that matches, in any directory
*.log
# a trailing / matches only directories
build/
# a leading / (or one in the middle) is relative to the folder
/secret.txt
docs/*.tmp
```

`*`, `?` and `[...]` work as in the shell; lines starting with `#` are
comments; `!` patterns aren't supported. Editor temp files (`*.swp`, `*~`,
`4913`, `.#*`) are always ignored.

`.isfignore` is synced too, so both sides use the same patterns, and changes
to it apply right away. Files synced before a pattern was added stay where
they are, but aren't synced anymore.

## Safety

- Files are written next to their place and renamed in, so a half-written
  file is never seen, and a file changed on the other side meanwhile is left
  alone instead of being overwritten.
- If a folder that was synced before is empty on one side when isf starts,
  isf stops instead of deleting everything on the other side. If that side
  was emptied by mistake, `isf --reset` copies everything back.
- Only one isf syncs a given folder to a given place at a time.
- If the connection drops, isf connects again when it can, and syncs what
  changed meanwhile on either side.
- `isf -n` shows what a sync would do without doing it.

## Where isf keeps its things

`~/.local/state/isf/` (or `$XDG_STATE_HOME/isf/`) holds what was last synced
for each folder, and where each folder syncs to (the `dest-*` files); delete
a `dest-*` file to make isf forget where a folder goes. While isf runs, the
ssh connection socket lives in `$XDG_RUNTIME_DIR/isf-*` (or `~/.ssh/isf-*`).

## Limitations

- A change made on the remote while isf wasn't running is found by size and
  modification time in whole seconds: an edit that kept the size, in the same
  second as the last sync, can be missed. Changes made while isf runs, and
  local changes, don't have this problem.
- A folder renamed while isf wasn't running is sent again, not renamed.
- isf runs in the foreground, one host at a time.
- Each side watches every folder it syncs, and Linux limits how many watches
  a user gets: a tree of many thousands of folders can run out on a small
  machine. isf says so, and how to raise the limit. (Ignored folders don't
  count: they aren't watched.)
- IPv6 addresses need a host alias in `~/.ssh/config`.
