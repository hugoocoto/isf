#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700 // nftw

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bt.h"
#include "pool.h"
#include "sync.h"
#include "util.h"

/* When both sides changed a file, the older version is kept with this suffix */
#define CONFLICT_SUFFIX ".isf-conflict"

typedef Da(char *) Names;
typedef Da(Record) Records;

static struct {
        Sftp *sftp;
        const char *host;
        const char *port;
        int dry_run;                   // plan and report, but don't do anything
        int sent, received, conflicts; // for sync_stats
} g;

void
sync_init(Sftp *sftp, const char *host, const char *port, char *const *ssh_opts, int jobs, int dry_run)
{
        g.sftp    = sftp;
        g.host    = host;
        g.port    = port;
        g.dry_run = dry_run;
        pool_start(dry_run ? 1 : jobs, host, port, ssh_opts);
}

SyncStats
sync_stats(void)
{
        return (SyncStats) { g.sent, g.received, g.conflicts };
}

/* REL as the user knows it: inside the folder's label, if it has one */
static char *
show_path(const Root *root, const char *rel)
{
        return (char *) (root->label && *root->label ? root_join(root->label, rel) : strdup(rel));
}

/* Tell what was synced: SENT to the remote, or received from it */
static void
report(int sent, const char *show, int is_dir, const char *what)
{
        printf("  %s %s%s%s%s\n", sent ? "↑" : "↓", show, is_dir ? "/" : "", what ? " " : "", what ? what : "");
        if (sent)
                g.sent++;
        else
                g.received++;
}

/* Directory part of a path */
static char *
parent_dir(const char *path)
{
        const char *slash = strrchr(path, '/');
        if (slash == NULL) return strdup(".");
        if (slash == path) return strdup("/");
        return strndup(path, slash - path);
}

/* Check an SFTP result: 1 if it worked, else log it and return 0. A broken
 * connection can't be recovered, so that exits. */
static int
sftp_ok(int st, const char *what, const char *remote)
{
        if (st == SFTP_OK) return 1;
        LOG("Error", "%s '%s:%s': %s", what, g.host, remote, g.sftp->error);
        if (st == SFTP_ERR_IO) exit(1);
        return 0;
}

static int
remote_is_dir(Sftp *c, const char *remote)
{
        SftpAttrs a;
        return sftp_lstat(c, remote, &a) == SFTP_OK && S_ISDIR(a.perm);
}

/* Like sftp_ok, but for a transfer on a worker connection: it never exits (the
 * pool reports a dead connection back to the main thread through c->dead). */
static int
conn_ok(Sftp *c, int st, const char *what, const char *remote)
{
        if (st == SFTP_OK) return 1;
        LOG("Error", "%s '%s:%s': %s", what, g.host, remote, c->error);
        return 0;
}

/* Mode and times. For directories only the mode: their mtime changes anyway
 * as things are created inside. */
static SftpAttrs
attrs_of(const struct stat *st)
{
        if (S_ISDIR(st->st_mode))
                return (SftpAttrs) { .flags = SFTP_ATTR_PERMISSIONS, .perm = st->st_mode & 07777 };
        return (SftpAttrs) {
                .flags = SFTP_ATTR_PERMISSIONS | SFTP_ATTR_ACMODTIME,
                .perm  = st->st_mode & 07777,
                .atime = st->st_atime,
                .mtime = st->st_mtime,
        };
}

static char
type_of(uint32_t mode)
{
        if (S_ISREG(mode)) return 'f';
        if (S_ISDIR(mode)) return 'd';
        if (S_ISLNK(mode)) return 'l';
        return '?';
}

static uint64_t
ctime_ns(const struct stat *st)
{
        return st->st_ctim.tv_sec * 1000000000ull + st->st_ctim.tv_nsec;
}

static State
local_state(const char *path)
{
        struct stat st;
        if (lstat(path, &st) == -1) {
                /* Anything but "not there" is unknown, not a deletion */
                if (errno == ENOENT || errno == ENOTDIR) return (State) { 0 };
                LOG_ERR("Cannot stat '%s'", path);
                return (State) { .type = '?' };
        }
        State s = {
                .type  = type_of(st.st_mode),
                .size  = st.st_size,
                .mtime = st.st_mtime,
                .mode  = st.st_mode & 07777,
                .stamp = ctime_ns(&st),
        };
        if (s.type == 'l') {
                char target[PATH_MAX];
                ssize_t n = readlink(path, target, sizeof target - 1);
                if (n == -1) return (State) { .type = '?' };
                target[n] = 0;
                s.link    = strdup(target);
        }
        return s;
}

/* Remote state of PATH from its attributes (from LSTAT or a listing) */
static State
remote_state_from(const char *path, const SftpAttrs *a)
{
        State s = {
                .type  = type_of(a->perm),
                .size  = a->size,
                .mtime = a->mtime,
                .mode  = a->perm & 07777,
        };
        if (s.type == 'l') {
                char target[PATH_MAX];
                int r = sftp_readlink(g.sftp, path, target, sizeof target);
                if (!sftp_ok(r, "Cannot read link", path)) return (State) { .type = '?' };
                s.link = strdup(target);
        }
        return s;
}

static State
remote_state(const char *path)
{
        SftpAttrs a;
        int r = sftp_lstat(g.sftp, path, &a);
        if (r == SFTP_NO_SUCH_FILE) return (State) { 0 };
        if (!sftp_ok(r, "Cannot stat", path)) return (State) { .type = '?' };
        return remote_state_from(path, &a);
}

static int
record_cmp(const void *a, const void *b)
{
        return strcmp(((const Record *) a)->rel, ((const Record *) b)->rel);
}

static Record *
record_find(Root *root, const char *rel)
{
        if (root->rec.count == 0) return NULL;
        Record key = { .rel = (char *) rel };
        return bsearch(&key, root->rec.items, root->rec.count, sizeof key, record_cmp);
}

/* Index of the first record that doesn't sort before REL */
static int
record_lower(const Root *root, const char *rel)
{
        int lo = 0, hi = root->rec.count;
        while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (strcmp(root->rec.items[mid].rel, rel) < 0)
                        lo = mid + 1;
                else
                        hi = mid;
        }
        return lo;
}

/* The records inside REL are [*LO, *HI). Everything starting with "REL/"
 * sorts together: from "REL/" to "REL0" ('0' comes right after '/'). */
static void
record_range(const Root *root, const char *rel, int *lo, int *hi)
{
        if (*rel == 0) { // everything but the root itself, which sorts first
                *lo = root->rec.count > 0 && root->rec.items[0].rel[0] == 0;
                *hi = root->rec.count;
                return;
        }
        size_t n  = strlen(rel);
        char *key = malloc(n + 2);
        assert(key);
        memcpy(key, rel, n);
        key[n + 1] = 0;
        key[n]     = '/';
        *lo        = record_lower(root, key);
        key[n]     = '/' + 1;
        *hi        = record_lower(root, key);
        free(key);
}

/* Take records [LO, HI) out of ROOT: appended to OUT if not NULL, else freed */
static void
record_take(Root *root, int lo, int hi, Records *out)
{
        if (lo >= hi) return;
        for (int i = lo; i < hi; i++) {
                Record *r = &root->rec.items[i];
                if (out) {
                        Da_append(out, *r);
                } else {
                        free(r->rel);
                        state_free(&r->st);
                }
        }
        memmove(root->rec.items + lo, root->rec.items + hi, (root->rec.count - hi) * sizeof *root->rec.items);
        root->rec.count -= hi - lo;
        root->rec_changed = 1;
}

/* Remove what's inside REL, and REL itself if SELF */
static void
record_del(Root *root, const char *rel, int self)
{
        int lo, hi;
        record_range(root, rel, &lo, &hi);
        record_take(root, lo, hi, NULL);
        Record *r = self ? record_find(root, rel) : NULL;
        if (r) record_take(root, Da_index(r, &root->rec), Da_index(r, &root->rec) + 1, NULL);
}

/* Record ST as the synced state of REL. Missing removes it. */
static void
record_set(Root *root, const char *rel, const State *st)
{
        if (st->type == 0) {
                record_del(root, rel, 1);
                return;
        }
        if (st->type != 'd') record_del(root, rel, 0); // it may have been a directory
        root->rec_changed = 1;

        Record *r = record_find(root, rel);
        if (r) {
                state_free(&r->st);
                r->st = state_copy(st);
                return;
        }
        /* Insert it where it keeps the array sorted (the index first:
         * Da_insert evaluates it after appending an empty record) */
        int at   = record_lower(root, rel);
        Record n = { .rel = strdup(rel), .st = state_copy(st) };
        Da_insert(&root->rec, n, at);
}

/* FROM was renamed to TO: so were the records inside it */
static void
record_rename(Root *root, const char *from, const char *to)
{
        record_del(root, to, 1);

        /* Take FROM and what's inside it out. Renamed, they are still in order
         * (they share the prefix), so they merge back in one pass. */
        Records moved = { 0 };
        Record *self  = record_find(root, from);
        if (self) record_take(root, Da_index(self, &root->rec), Da_index(self, &root->rec) + 1, &moved);
        int lo, hi;
        record_range(root, from, &lo, &hi);
        record_take(root, lo, hi, &moved);
        if (moved.count == 0) return;

        size_t from_len = strlen(from);
        Da_foreach(r, moved)
        {
                const char *rest = r->rel + from_len;
                char *rel        = malloc(strlen(to) + strlen(rest) + 1);
                assert(rel);
                strcpy(stpcpy(rel, to), rest);
                free(r->rel);
                r->rel = rel;
        }

        Records all = { 0 };
        int i = 0, j = 0;
        while (i < root->rec.count || j < moved.count) {
                if (j == moved.count || (i < root->rec.count && strcmp(root->rec.items[i].rel, moved.items[j].rel) < 0))
                        Da_append(&all, root->rec.items[i++]);
                else
                        Da_append(&all, moved.items[j++]);
        }
        Da_destroy(&root->rec);
        Da_destroy(&moved);
        root->rec.items    = all.items;
        root->rec.count    = all.count;
        root->rec.capacity = all.capacity;
}

/* Append to NAMES the names of what's recorded directly inside REL */
static void
record_children(Root *root, const char *rel, Names *names)
{
        int lo, hi;
        record_range(root, rel, &lo, &hi);
        size_t n = *rel ? strlen(rel) + 1 : 0;
        for (int i = lo; i < hi; i++) {
                const char *name = root->rec.items[i].rel + n;
                if (strchr(name, '/') == NULL) Da_append(names, strdup(name));
        }
}

/* The record of a root is saved in $XDG_STATE_HOME/isf (or
 * ~/.local/state/isf), named with a hash of what it syncs */
static char *
record_path(const Root *root)
{
        char *abs = realpath(root->local, NULL);
        if (abs == NULL) return NULL;
        uint64_t hash = HASH_INIT;
        hash          = hash_str(hash, g.host);
        hash          = hash_str(hash, g.port ? g.port : "");
        hash          = hash_str(hash, root->remote);
        hash          = hash_str(hash, abs);
        free(abs);

        char name[17];
        snprintf(name, sizeof name, "%016llx", (unsigned long long) hash);
        return state_path(name);
}

/* Each record is "TYPE SIZE MTIME MODE STAMP:" (mode in octal), the path,
 * NUL, the link target (empty if none), NUL */
static void
record_load(Root *root)
{
        FILE *f = fopen(root->rec_path, "r");
        if (f == NULL) return; // never synced
        CharBuf buf = { 0 };
        int c;
        while ((c = fgetc(f)) != EOF)
                Da_append(&buf, c);
        Da_append(&buf, 0);
        fclose(f);

        for (int pos = 0; pos < buf.count - 1;) {
                char type;
                unsigned long long size, stamp;
                unsigned mtime, mode;
                int n = 0;
                if (sscanf(buf.items + pos, "%c %llu %u %o %llu:%n", &type, &size, &mtime, &mode, &stamp, &n) != 5 || n == 0) {
                        LOG_WARN("'%s' is damaged, ignoring the rest of it", root->rec_path);
                        break;
                }
                const char *rel  = buf.items + pos + n;
                const char *link = rel + strlen(rel) + 1;
                if (link - buf.items >= buf.count) break;
                pos = link - buf.items + strlen(link) + 1;
                Da_append(&root->rec, ((Record) {
                                      .rel = strdup(rel),
                                      .st  = {
                                      .type  = type,
                                      .size  = size,
                                      .mtime = mtime,
                                      .mode  = mode,
                                      .link  = *link ? strdup(link) : NULL,
                                      .stamp = stamp,
                                      },
                                      }));
        }
        Da_destroy(&buf);
        if (root->rec.count) qsort(root->rec.items, root->rec.count, sizeof *root->rec.items, record_cmp);
}

void
record_save(Root *root)
{
        if (!root->rec_changed) return;
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s.tmp", root->rec_path);
        FILE *f = fopen(tmp, "w");
        if (f == NULL) {
                LOG_ERR("Cannot save '%s'", tmp);
                return;
        }
        Da_foreach(r, root->rec)
        {
                fprintf(f, "%c %llu %u %o %llu:%s", r->st.type, (unsigned long long) r->st.size,
                        r->st.mtime, r->st.mode, (unsigned long long) r->st.stamp, r->rel);
                fputc(0, f);
                fputs(r->st.link ? r->st.link : "", f);
                fputc(0, f);
        }
        if (fclose(f) == 0 && rename(tmp, root->rec_path) == 0)
                root->rec_changed = 0;
        else
                LOG_ERR("Cannot save '%s'", root->rec_path);
}


/* Send a regular file. It's written to a temp file next to it and renamed
 * over, so the remote file is never seen half written. */
static int
send_file(Sftp *c, const char *local, const char *remote, int worker, State *sent)
{
        int fd = open(local, O_RDONLY | O_CLOEXEC);
        if (fd == -1) {
                /* ENOENT: removed since, and that has its own event */
                if (errno != ENOENT) LOG_ERR("Cannot open '%s'", local);
                return 0;
        }
        struct stat st;
        if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
                close(fd);
                return 0;
        }

        char *parent    = parent_dir(remote);
        const char *tmp = temp_path(parent, worker);
        SftpAttrs attrs = attrs_of(&st);

        int r = sftp_put(c, fd, tmp, &attrs);
        if (r == SFTP_NO_SUCH_FILE) {
                /* The remote folder is missing */
                r = sftp_mkdir_p(c, parent);
                if (r == SFTP_OK && lseek(fd, 0, SEEK_SET) == 0)
                        r = sftp_put(c, fd, tmp, &attrs);
        }
        if (conn_ok(c, r, "Cannot upload", remote)) {
                r = sftp_rename(c, tmp, remote);
                if (r != SFTP_OK && r != SFTP_ERR_IO && remote_is_dir(c, remote)) {
                        /* The file replaced a directory */
                        conn_ok(c, sftp_remove_all(c, remote), "Cannot remove", remote);
                        r = sftp_rename(c, tmp, remote);
                }
                conn_ok(c, r, "Cannot rename to", remote);
        }
        if (r != SFTP_OK) sftp_remove(c, tmp); // may not exist
        if (r == SFTP_OK)
                *sent = (State) {
                        .type  = 'f',
                        .size  = st.st_size,
                        .mtime = st.st_mtime,
                        .mode  = st.st_mode & 07777,
                        .stamp = ctime_ns(&st),
                };

        close(fd);
        free(parent);
        free((void *) tmp);
        return r == SFTP_OK;
}

static int
send_symlink(const char *local, const char *remote, State *sent)
{
        char target[PATH_MAX];
        ssize_t n = readlink(local, target, sizeof target - 1);
        if (n == -1) {
                if (errno != ENOENT) LOG_ERR("Cannot read link '%s'", local);
                return 0;
        }
        target[n] = 0;

        /* SYMLINK doesn't replace what's there */
        int r = sftp_remove_all(g.sftp, remote);
        if (r != SFTP_NO_SUCH_FILE && !sftp_ok(r, "Cannot remove", remote)) return 0;

        r = sftp_symlink(g.sftp, target, remote);
        if (r == SFTP_NO_SUCH_FILE) {
                /* The remote folder is missing */
                char *parent = parent_dir(remote);
                r            = sftp_mkdir_p(g.sftp, parent);
                if (r == SFTP_OK) r = sftp_symlink(g.sftp, target, remote);
                free(parent);
        }
        if (!sftp_ok(r, "Cannot create symlink", remote)) return 0;
        *sent = (State) { .type = 'l', .link = strdup(target) };
        return 1;
}

static int
remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
        Unused(st, flag, ftw);
        if (remove(path) == -1 && errno != ENOENT) LOG_ERR("Cannot remove '%s'", path);
        return 0;
}

/* rm -rf */
static void
local_remove_all(const char *path)
{
        if (nftw(path, remove_one, 16, FTW_DEPTH | FTW_PHYS) == -1 && errno != ENOENT)
                LOG_ERR("Cannot remove '%s'", path);
}

/* Does the local PATH still have PLANNED, what it had when its step was
 * planned? If not, it changed meanwhile and isn't replaced: its events are on
 * the way, and the next round sees the change. */
static int
local_unchanged(const char *path, const State *planned)
{
        State now = local_state(path);
        int same_now = same_local(&now, planned);
        state_free(&now);
        if (!same_now) VPRINT("File: %s [changed meanwhile, left alone]\n", path);
        return same_now;
}

/* What to record for WANT, just fetched into LOCAL: its ctime too, so it
 * doesn't look like a local change */
static State
fetched(const char *local, const State *want)
{
        State done = state_copy(want);
        struct stat st;
        if (done.type == 'f' && lstat(local, &st) == 0) done.stamp = ctime_ns(&st);
        return done;
}

/* Fetch a regular file into a temp file, renamed over LOCAL if it still has
 * EXPECT then */
static int
fetch_file(Sftp *c, const char *remote, const char *local, int worker, const State *want, const State *expect)
{
        char *parent    = parent_dir(local);
        const char *tmp = temp_path(parent, worker);
        int ok          = 0;
        int fd          = -1;
        if (mkdir_p(parent) == -1) {
                LOG_ERR("Cannot create '%s'", parent);
                goto out;
        }
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd == -1) {
                LOG_ERR("Cannot create '%s'", tmp);
                goto out;
        }
        ok = conn_ok(c, sftp_get(c, remote, fd), "Cannot download", remote);

        /* The same mode and mtime as the remote, so both look the same */
        struct timespec times[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = want->mtime } };
        if (ok && (fchmod(fd, want->mode) == -1 || futimens(fd, times) == -1)) {
                LOG_ERR("Cannot set the mode or mtime of '%s'", tmp);
                ok = 0;
        }
        close(fd);

        if (ok && !local_unchanged(local, expect)) ok = 0;
        if (ok) {
                struct stat st;
                if (lstat(local, &st) == 0 && S_ISDIR(st.st_mode)) local_remove_all(local); // the file replaced it
                if (rename(tmp, local) == -1) {
                        LOG_ERR("Cannot rename to '%s'", local);
                        ok = 0;
                }
        }
        if (!ok) unlink(tmp);
out:
        free(parent);
        free((void *) tmp);
        return ok;
}

static int
fetch_symlink(const char *local, const State *want)
{
        char *parent = parent_dir(local);
        int ok       = mkdir_p(parent) == 0;
        free(parent);
        local_remove_all(local); // symlink() doesn't replace
        if (ok && symlink(want->link, local) == 0) return 1;
        LOG_ERR("Cannot create symlink '%s'", local);
        return 0;
}

static char *
conflict_name(const char *path)
{
        char *copy = malloc(strlen(path) + sizeof CONFLICT_SUFFIX);
        assert(copy);
        strcpy(stpcpy(copy, path), CONFLICT_SUFFIX);
        return copy;
}

/* Keep the version that lost next to the winner */
static int
keep_remote_copy(const char *remote)
{
        char *copy = conflict_name(remote);
        int ok     = sftp_ok(sftp_rename(g.sftp, remote, copy), "Cannot keep a copy of", remote);
        free(copy);
        return ok;
}

static int
keep_local_copy(const char *local)
{
        char *copy = conflict_name(local);
        int ok     = rename(local, copy) == 0;
        if (!ok) LOG_ERR("Cannot keep a copy of '%s'", local);
        free(copy);
        return ok;
}

/* ---- transfers ------------------------------------------------------------
 * The bytes of files are independent of everything else, so they go to the
 * transfer pool when there is one. They are recorded and reported when they
 * come back (sync_drain), here on the main thread. */

typedef struct {
        int up;
        Root *root;
        char *rel, *local, *remote, *show;
        State want;   // down: what the remote has, recorded once fetched
        State expect; // down: what the local side has to still have
        State result; // up: what was sent
        int ok;
} Transfer;

static void
transfer_run(Sftp *c, int worker, void *arg)
{
        Transfer *t = arg;
        if (t->up)
                t->ok = send_file(c, t->local, t->remote, worker, &t->result);
        else
                t->ok = fetch_file(c, t->remote, t->local, worker, &t->want, &t->expect);
}

static void
transfer_done(void *arg)
{
        Transfer *t = arg;
        if (t->ok) {
                State done = t->up ? state_copy(&t->result) : fetched(t->local, &t->want);
                record_set(t->root, t->rel, &done);
                report(t->up, t->show, 0, NULL);
                state_free(&done);
        }
        state_free(&t->want);
        state_free(&t->expect);
        state_free(&t->result);
        free(t->rel);
        free(t->local);
        free(t->remote);
        free(t->show);
        free(t);
}

static int
entry_cmp(const void *a, const void *b)
{
        return strcmp(((const SftpEntry *) a)->name, ((const SftpEntry *) b)->name);
}

/* DIR has to be sorted with entry_cmp */
static const SftpEntry *
find_entry(const SftpDir *dir, const char *name)
{
        if (dir->count == 0) return NULL;
        SftpEntry key = { .name = (char *) name };
        return bsearch(&key, dir->items, dir->count, sizeof *dir->items, entry_cmp);
}

/* Was ROOT synced before, but now one side is empty? Most likely a wrong
 * folder or a wiped disk: syncing would delete everything on the other side. */
static int
looks_wiped(Root *root)
{
        if (root->rec.count <= 1) return 0; // only the folder itself

        int local_empty = 1;
        DIR *dir        = opendir(root->local);
        if (dir) {
                struct dirent *e;
                while (local_empty && (e = readdir(dir)))
                        local_empty = !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..");
                closedir(dir);
        }
        SftpDir have     = { 0 };
        int r            = sftp_readdir(g.sftp, root->remote, &have);
        int remote_empty = r == SFTP_NO_SUCH_FILE || (r == SFTP_OK && have.count == 0);
        sftp_ok(r == SFTP_NO_SUCH_FILE ? SFTP_OK : r, "Cannot list", root->remote);
        sftp_dir_free(&have);

        if (local_empty == remote_empty) return 0;
        const char *empty = local_empty ? root->local : root->remote;
        const char *other = local_empty ? "the remote" : root->local;
        LOG("Error", "'%s%s%s' is empty, but it was synced before: syncing now would delete everything in %s. "
                     "If it was emptied by mistake, run isf again with --reset and it's copied back.",
            local_empty ? "" : g.host, local_empty ? "" : ":", empty, other);
        return 1;
}

static int
name_cmp(const void *a, const void *b)
{
        return strcmp(*(char *const *) a, *(char *const *) b);
}

/* ---- remote listings, read ahead -----------------------------------------
 * Walking a tree needs the remote listing of each directory in it. Asked for
 * one directory at a time, that costs a round trip or two per directory, which
 * adds up in deep trees. So the first time a walk needs a listing, the whole
 * remote tree below it is listed level by level, every directory of a level in
 * one pipelined batch: a few round trips per level instead. children() takes
 * the listings from here, and they are dropped when the walk ends. */

/* Entries listed ahead at most. The directories below that are listed when
 * the walk gets there, and ahead again from there. */
#define READAHEAD_MAX 100000

typedef struct {
        int listed;  // else a directory seen in its parent's listing, not listed yet
        int status;  // of listing it: SFTP_OK, SFTP_NO_SUCH_FILE (empty) or a failure
        int taken;   // by children()
        SftpDir dir; // its entries, sorted by name
} Listing;

static struct {
        BT map; // remote path -> Listing *
        /* To free them. Not by iterating the map: bt.h has one iterator, and
         * main.c is using it while it calls reconcile. */
        Da(Listing *) all;
} ahead;

static Listing *
listing(const char *remote)
{
        Listing *l = bt_get(&ahead.map, remote);
        if (l) return l;
        l = calloc(1, sizeof *l);
        assert(l);
        bt_add(&ahead.map, remote, l);
        Da_append(&ahead.all, l);
        return l;
}

static void
ahead_forget(void)
{
        Da_foreach(l, ahead.all)
        {
                sftp_dir_free(&(*l)->dir);
                free(*l);
        }
        Da_destroy(&ahead.all);
        bt_destroy(&ahead.map);
}

static void
names_free(Names *names)
{
        Da_foreach(n, *names)
        {
                free(*n);
        }
        Da_destroy(names);
}

/* List REMOTE, which is REL inside ROOT, and the directories below it (up to
 * READAHEAD_MAX entries) into ahead, a level at a time. What children() would
 * skip isn't listed. */
static void
read_ahead(Root *root, const char *rel, const char *remote)
{
        Names level = { 0 }, rels = { 0 }; // remote paths, and their REL
        Da_append(&level, strdup(remote));
        Da_append(&rels, strdup(rel));
        int entries = 0;

        while (level.count > 0) {
                SftpDir *out = calloc(level.count, sizeof *out);
                int *status  = calloc(level.count, sizeof *status);
                assert(out && status);
                sftp_ok(sftp_readdir_many(g.sftp, level.items, level.count, out, status), "Cannot list", remote);

                Names next = { 0 }, next_rels = { 0 };
                for (int i = 0; i < level.count; i++) {
                        Listing *l = listing(level.items[i]);
                        sftp_dir_free(&l->dir);
                        *l = (Listing) { .listed = 1, .status = status[i], .dir = out[i] };
                        entries += l->dir.count;
                        if (l->dir.count) qsort(l->dir.items, l->dir.count, sizeof *l->dir.items, entry_cmp);
                        Da_foreach(e, l->dir)
                        {
                                if (!S_ISDIR(e->attrs.perm) || !name_safe(e->name) || is_temp_name(e->name)) continue;
                                const char *child = *rels.items[i] ? pathjoin(rels.items[i], e->name) : strdup(e->name);
                                if (ignored(&root->ign, child, 1)) {
                                        free((void *) child);
                                        continue;
                                }
                                Da_append(&next, (char *) pathjoin(level.items[i], e->name));
                                Da_append(&next_rels, (char *) child);
                        }
                }
                free(out);
                free(status);
                names_free(&level);
                names_free(&rels);
                level = next;
                rels  = next_rels;

                if (entries >= READAHEAD_MAX) {
                        /* Enough for now: these are listed when the walk gets there */
                        Da_foreach(p, level)
                        {
                                listing(*p);
                        }
                        names_free(&level);
                        names_free(&rels);
                }
        }
}

/* Put the remote listing of REMOTE, which is REL inside ROOT, sorted by name,
 * in *DIR (free it with sftp_dir_free). Empty if REMOTE is missing. Returns
 * SFTP_OK, or why it couldn't be listed. */
static int
take_listing(Root *root, const char *rel, const char *remote, SftpDir *dir)
{
        *dir       = (SftpDir) { 0 };
        Listing *l = bt_get(&ahead.map, remote);
        if (l == NULL) {
                /* Its parent was listed without it as a directory: it's
                 * missing on the remote, or was just made there, empty */
                char *parent = parent_dir(remote);
                Listing *p   = bt_get(&ahead.map, parent);
                free(parent);
                if (p && p->listed) return SFTP_OK;
        }
        if (l == NULL || !l->listed || l->taken) {
                read_ahead(root, rel, remote);
                l = bt_get(&ahead.map, remote);
        }
        l->taken = 1;
        if (l->status != SFTP_OK && l->status != SFTP_NO_SUCH_FILE) {
                /* List it on its own, for the reason */
                int r = sftp_readdir(g.sftp, remote, dir);
                if (r == SFTP_OK || r == SFTP_NO_SUCH_FILE) {
                        if (dir->count) qsort(dir->items, dir->count, sizeof *dir->items, entry_cmp);
                        return SFTP_OK;
                }
                sftp_dir_free(dir);
                return r;
        }
        *dir   = l->dir;
        l->dir = (SftpDir) { 0 };
        return SFTP_OK;
}

/* ---- the walk: planning -----------------------------------------------------
 * A walk over a tree: the plan, and how much of it is done. Steps are done
 * as soon as they are planned (apply_ready), so transfers start while the
 * rest is planned; a directory's own last steps are planned after what's
 * inside it, from what was planned for that. */
typedef struct {
        Root *root;
        Plan plan;
        int done;     // steps done so far
        Names failed; // paths whose steps failed
} Walk;

static int plan_path(Walk *w, const char *rel, int what, const State *remote_now);
static void apply_ready(Walk *w);

/* Plan everything directly inside REL, and below: what either side has, and
 * what the record has (gone from one side or both since) */
static void
plan_children(Walk *w, const char *rel)
{
        Root *root         = w->root;
        const char *local  = root_join(root->local, rel);
        const char *remote = root_join(root->remote, rel);
        Names names        = { 0 };
        SftpDir have;

        /* Without the remote listing everything there would look deleted */
        if (!sftp_ok(take_listing(root, rel, remote, &have), "Cannot list", remote)) goto out;
        Da_foreach(e, have)
        {
                Da_append(&names, strdup(e->name));
        }

        DIR *dir = opendir(local);
        if (dir) {
                struct dirent *e;
                while ((e = readdir(dir))) {
                        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                                Da_append(&names, strdup(e->d_name));
                }
                closedir(dir);
        }
        record_children(root, rel, &names);
        if (names.count) qsort(names.items, names.count, sizeof *names.items, name_cmp);

        for (int i = 0; i < names.count; i++) {
                const char *name = names.items[i];
                if (i > 0 && !strcmp(name, names.items[i - 1])) continue; // in several lists
                if (is_temp_name(name)) continue;
                /* A remote listing could hold a crafted name ("..", "a/b") that
                 * would reach outside the folder */
                if (!name_safe(name)) {
                        LOG_WARN("Skipping an unsafe name from the remote: '%s'", name);
                        continue;
                }
                const SftpEntry *e = find_entry(&have, name);

                /* A directory on either side counts as one for the patterns */
                int is_dir = e && S_ISDIR(e->attrs.perm);
                if (!is_dir) {
                        const char *cl = pathjoin(local, name);
                        struct stat st;
                        is_dir = lstat(cl, &st) == 0 && S_ISDIR(st.st_mode);
                        free((void *) cl);
                }

                const char *child = *rel ? pathjoin(rel, name) : strdup(name);
                if (!ignored(&root->ign, child, is_dir)) {
                        const char *child_remote = pathjoin(remote, name);
                        State now                = e ? remote_state_from(child_remote, &e->attrs) : (State) { 0 };
                        plan_path(w, child, SYNC_TREE, &now);
                        apply_ready(w);
                        state_free(&now);
                        free((void *) child_remote);
                }
                free((void *) child);
        }

out:
        names_free(&names);
        sftp_dir_free(&have);
        free((void *) local);
        free((void *) remote);
}

/* ---- the walk: doing the steps ----------------------------------------------
 * Directory modes are set last, when the transfers into them are done: a
 * read-only directory can't be written into */
typedef struct {
        int up;
        char *path;
        uint32_t mode;
} DirMode;
static Da(DirMode) dir_modes;

static void
apply_dir_modes(void)
{
        Da_foreach(d, dir_modes)
        {
                if (d->up) {
                        SftpAttrs a = { .flags = SFTP_ATTR_PERMISSIONS, .perm = d->mode };
                        sftp_ok(sftp_setstat(g.sftp, d->path, &a), "Cannot set the mode of", d->path);
                } else if (chmod(d->path, d->mode) == -1) {
                        LOG_ERR("Cannot set the mode of '%s'", d->path);
                }
                free(d->path);
        }
        dir_modes.count = 0;
}

/* Does a step in PLAN, from START on, leave something on the side UP names? */
static int
plan_keeps(const Plan *plan, int start, int up)
{
        for (int i = start; i < plan->count; i++) {
                const Action *a = &plan->items[i];
                if (a->type == ACT_RECORD ? a->st.type != 0 :
                                            a->up == up && (a->type == ACT_COPY || a->type == ACT_MKDIR || a->type == ACT_MODE))
                        return 1;
        }
        return 0;
}

/* REL is a directory on at least one side. L and M are what the local side
 * and the remote have, R the record. Returns 1 if what's inside was planned. */
static int
plan_dir(Walk *w, const char *rel, const State *L, const State *M, const State *R, int deep)
{
        Plan *plan = &w->plan;
        int ld     = L->type == 'd';
        DirCase dc = decide_dir(L, M, R);

        if (dc == DIR_BOTH) {
                int is_new = R->type != 'd';
                if (deep || is_new) plan_children(w, rel);
                /* Last: a read-only mode would stop the steps inside */
                State st = *L;
                uint32_t mode;
                int up;
                if (dir_mode(L, M, R, &mode, &up)) {
                        st.mode = mode;
                        plan_add(plan, ACT_MODE, up, rel, L, M, &st);
                }
                plan_add(plan, ACT_RECORD, 0, rel, L, M, &st);
                return deep || is_new;
        }
        if (*rel == 0) {
                LOG_WARN("'%s' is missing on one side, not touching it", w->root->local);
                return 1;
        }

        const State *D = ld ? L : M; // the directory
        const State *O = ld ? M : L; // what the other side has instead
        switch (dc) {
        case DIR_LOST:
                plan_add(plan, ACT_COPY, !ld, rel, L, M, NULL);
                break;
        case DIR_GONE: {
                /* Going through what's inside, what changed here since is
                 * kept (an edit beats a deletion) and the rest is removed. If
                 * nothing is kept, the directory goes too. */
                int start = plan->count;
                plan_children(w, rel);
                if (plan_keeps(plan, start, ld)) {
                        plan_add(plan, ACT_MODE, ld, rel, L, M, D); // made again for what's kept
                        plan->items[plan->count - 1].quiet = 1;
                        plan_add(plan, ACT_RECORD, 0, rel, L, M, D);
                } else {
                        plan_add(plan, ACT_RMDIR, !ld, rel, L, M, D);
                }
                break;
        }
        default: // DIR_WINS
                /* The file it replaces is kept if it changed */
                plan_add(plan, ACT_MKDIR, ld, rel, L, M, D);
                plan->items[plan->count - 1].keep = O->type == 'f' && !same(O, R);
                plan_children(w, rel);
                plan_add(plan, ACT_MODE, ld, rel, L, M, D);
                plan->items[plan->count - 1].quiet = 1;
                plan_add(plan, ACT_RECORD, 0, rel, L, M, D);
        }
        return 1;
}

/* Plan REL: for a directory, also what's inside if the events said so
 * (SYNC_TREE in WHAT) or it's new. REMOTE_NOW is the remote state if known.
 * Returns 1 if everything inside was planned. */
static int
plan_path(Walk *w, const char *rel, int what, const State *remote_now)
{
        Root *root         = w->root;
        const char *local  = root_join(root->local, rel);
        const char *remote = root_join(root->remote, rel);
        State L            = local_state(local);
        State M            = remote_now ? state_copy(remote_now) : remote_state(remote);
        Record *rec        = record_find(root, rel);
        State R            = rec ? state_copy(&rec->st) : (State) { 0 };
        int covered        = 0;

        /* A dry run doesn't make the remote folder: as if it was there, empty */
        if (g.dry_run && *rel == 0 && L.type == 'd' && M.type == 0) M = (State) { .type = 'd', .mode = L.mode };

        if (L.type == '?' || M.type == '?')
                LOG_WARN("Skipping '%s': not a file, directory or symlink on one side", local);
        else if (L.type == 'd' || M.type == 'd')
                covered = plan_dir(w, rel, &L, &M, &R, what & SYNC_TREE);
        else
                plan_file(&w->plan, rel, &L, &M, &R, what);

        state_free(&L);
        state_free(&M);
        state_free(&R);
        free((void *) local);
        free((void *) remote);
        return covered;
}

/* Do step A. Returns 0 if it failed: then the steps for what's inside it are
 * skipped. */
static int
apply_step(Root *root, const Action *a)
{
        const char *local  = root_join(root->local, a->rel);
        const char *remote = root_join(root->remote, a->rel);
        char *show         = show_path(root, a->rel);
        const State none   = { 0 };
        int ok             = 1;

        switch (a->type) {
        case ACT_RECORD:
                if (!g.dry_run) record_set(root, a->rel, &a->st);
                break;

        case ACT_COPY: {
                const State *W = a->up ? &a->L : &a->M; // what wins
                const State *O = a->up ? &a->M : &a->L; // what it replaces
                int keep       = a->conflict && O->type == 'f';
                if (a->conflict) {
                        printf("  ! %s changed on both sides: kept the newest", show);
                        if (keep)
                                printf(", the other one is %s%s\n", show, CONFLICT_SUFFIX);
                        else
                                printf("\n");
                        g.conflicts++;
                }
                if (g.dry_run) {
                        report(a->up, show, 0, NULL);
                        break;
                }
                if (!a->up && !(ok = local_unchanged(local, &a->L))) break;
                if (keep && !(ok = a->up ? keep_remote_copy(remote) : keep_local_copy(local))) break;
                const State *expect = keep ? &none : &a->L; // down: what the local side has now

                if (W->type == 'f' && pool_enabled()) {
                        Transfer *t = calloc(1, sizeof *t);
                        assert(t);
                        *t = (Transfer) {
                                .up     = a->up,
                                .root   = root,
                                .rel    = strdup(a->rel),
                                .local  = strdup(local),
                                .remote = strdup(remote),
                                .show   = strdup(show),
                                .want   = state_copy(&a->M),
                                .expect = state_copy(expect),
                        };
                        pool_submit(transfer_run, t);
                        break;
                }
                State done = { 0 };
                if (a->up) {
                        ok = W->type == 'f' ? send_file(g.sftp, local, remote, 0, &done) : send_symlink(local, remote, &done);
                } else {
                        ok = W->type == 'f' ? fetch_file(g.sftp, remote, local, 0, &a->M, expect) : fetch_symlink(local, &a->M);
                        if (ok) done = fetched(local, &a->M);
                }
                if (ok) {
                        report(a->up, show, 0, NULL);
                        record_set(root, a->rel, &done);
                }
                state_free(&done);
                break;
        }

        case ACT_ATTRS:
                if (g.dry_run) {
                        report(a->up, show, 0, "mode");
                } else if (a->up) {
                        SftpAttrs at = {
                                .flags = SFTP_ATTR_PERMISSIONS | SFTP_ATTR_ACMODTIME,
                                .perm  = a->L.mode,
                                .atime = a->L.mtime,
                                .mtime = a->L.mtime,
                        };
                        ok = sftp_ok(sftp_setstat(g.sftp, remote, &at), "Cannot set the mode of", remote);
                        if (ok) {
                                report(1, show, 0, "mode");
                                record_set(root, a->rel, &a->L);
                        }
                } else if ((ok = local_unchanged(local, &a->L))) {
                        ok = chmod(local, a->M.mode) == 0;
                        if (!ok) {
                                LOG_ERR("Cannot set the mode of '%s'", local);
                                break;
                        }
                        report(0, show, 0, "mode");
                        State done = fetched(local, &a->M);
                        record_set(root, a->rel, &done);
                        state_free(&done);
                }
                break;

        case ACT_REMOVE:
                if (g.dry_run) {
                        if ((a->up ? &a->M : &a->L)->type) report(a->up, show, 0, "deleted");
                        break;
                }
                if (a->up) {
                        int r = sftp_remove_all(g.sftp, remote);
                        if (r == SFTP_OK) report(1, show, 0, "deleted");
                        ok = r == SFTP_NO_SUCH_FILE || sftp_ok(r, "Cannot remove", remote);
                } else if ((ok = local_unchanged(local, &a->L))) {
                        local_remove_all(local);
                        if (a->L.type) report(0, show, 0, "deleted");
                }
                if (ok) record_set(root, a->rel, &none);
                break;

        case ACT_MKDIR: {
                const State *O = a->up ? &a->M : &a->L; // in the way
                if (g.dry_run) {
                        report(a->up, show, 1, NULL);
                        break;
                }
                if (O->type != 0) {
                        if (!a->up && !(ok = local_unchanged(local, &a->L))) break;
                        if (a->keep) {
                                ok = a->up ? keep_remote_copy(remote) : keep_local_copy(local);
                        } else if (a->up) {
                                int r = sftp_remove_all(g.sftp, remote);
                                ok    = r == SFTP_NO_SUCH_FILE || sftp_ok(r, "Cannot remove", remote);
                        } else {
                                local_remove_all(local);
                        }
                        if (!ok) break;
                }
                if (a->up) {
                        ok = sftp_ok(sftp_mkdir_p(g.sftp, remote), "Cannot create", remote);
                } else if (!(ok = mkdir_p(local) == 0)) {
                        LOG_ERR("Cannot create '%s'", local);
                }
                if (ok) report(a->up, show, 1, NULL);
                break;
        }

        case ACT_RMDIR: {
                if (g.dry_run) {
                        report(a->up, show, 1, "deleted");
                        break;
                }
                int gone;
                if (a->up) {
                        int r = sftp_rmdir(g.sftp, remote);
                        if (r == SFTP_ERR_IO) sftp_ok(r, "Cannot remove", remote); // exits
                        gone = r == SFTP_OK;
                } else {
                        gone = rmdir(local) == 0;
                }
                if (gone) report(a->up, show, 1, "deleted");
                /* Else something inside couldn't be synced: it stays */
                record_set(root, a->rel, gone ? &none : &a->st);
                break;
        }

        case ACT_MODE:
                if (!a->quiet) report(a->up, show, 1, "mode");
                if (!g.dry_run)
                        Da_append(&dir_modes, (DirMode) { .up = a->up, .path = strdup(a->up ? remote : local), .mode = a->st.mode });
                break;
        }

        free(show);
        free((void *) local);
        free((void *) remote);
        return ok;
}

/* Do the steps planned since the last time, in order. When one fails, the
 * steps for the same path and what's inside it are skipped: they need it. */
static void
apply_ready(Walk *w)
{
        for (; w->done < w->plan.count; w->done++) {
                const Action *a = &w->plan.items[w->done];
                int skip        = 0;
                Da_foreach(f, w->failed)
                {
                        if (!strcmp(a->rel, *f) || inside(a->rel, *f)) skip = 1;
                }
                if (!skip && !apply_step(w->root, a)) Da_append(&w->failed, strdup(a->rel));
        }
}

int
reconcile(Root *root, const char *rel, int what, const State *remote_now)
{
        Walk w      = { .root = root };
        int covered = plan_path(&w, rel, what, remote_now);
        apply_ready(&w);
        ahead_forget(); // the listings read ahead were for this walk
        plan_free(&w.plan);
        names_free(&w.failed);
        return covered;
}

int
sync_drain(void)
{
        int died = pool_drain(transfer_done);
        apply_dir_modes(); // now that what goes inside is in
        return died;
}

void
sync_shutdown(void)
{
        pool_stop();
}

/* Hold an exclusive lock on the record's sidecar for as long as isf runs, so
 * a second isf syncing the same folder to the same place can't race this one.
 * The lock is released when the process exits. */
static int
lock_record(Root *root)
{
        char *path = malloc(strlen(root->rec_path) + sizeof ".lock");
        assert(path);
        sprintf(path, "%s.lock", root->rec_path);
        /* O_CLOEXEC so the ssh children don't keep the lock after isf exits */
        int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        free(path);
        if (fd == -1) {
                LOG_ERR("Cannot open the lock for '%s'", root->local);
                return 1;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
                int e = errno;
                close(fd);
                if (e == EWOULDBLOCK) {
                        LOG("Error", "'%s' is already being synced to %s:%s by another isf; stop it first",
                            root->local, g.host, root->remote);
                        return 1;
                }
                errno = e;
                LOG_ERR("Cannot lock '%s'", root->local);
                return 1;
        }
        root->lock_fd = fd;
        return 0;
}

int
sync_open(Root *root, int reset)
{
        root->rec_path = record_path(root);
        if (root->rec_path == NULL) {
                LOG_ERR("Cannot find where to keep the record of '%s'", root->local);
                return 1;
        }
        if (lock_record(root)) return 1;
        if (!reset)
                record_load(root);
        else if (!g.dry_run && unlink(root->rec_path) == -1 && errno != ENOENT)
                LOG_ERR("Cannot remove '%s'", root->rec_path);
        ignore_load(&root->ign, root->local);
        return looks_wiped(root);
}

int
sync_rename(Root *root, const char *from, const char *to)
{
        const char *local_to    = root_join(root->local, to);
        const char *remote_from = root_join(root->remote, from);
        const char *remote_to   = root_join(root->remote, to);
        Record *rec             = record_find(root, from);
        int renamed             = 0;

        /* Only a plain rename: it's still what we last synced, and the remote
         * didn't change it. Our own renames (conflict copies) aren't. */
        if (rec) {
                State L = local_state(local_to);
                State M = remote_state(remote_from);
                if (same(&L, &rec->st) && same(&M, &rec->st)) {
                        int r = sftp_rename(g.sftp, remote_from, remote_to);
                        if (r == SFTP_OK) {
                                char *show_from = show_path(root, from), *show_to = show_path(root, to);
                                printf("  ↑ %s → %s\n", show_from, show_to);
                                g.sent++;
                                free(show_from);
                                free(show_to);
                                record_rename(root, from, to);
                                /* The rename moved its ctime */
                                if (L.type == 'f') record_set(root, to, &L);
                                renamed = 1;
                        } else if (r != SFTP_NO_SUCH_FILE) {
                                sftp_ok(r, "Cannot rename", remote_from);
                        }
                }
                state_free(&L);
                state_free(&M);
        }
        free((void *) local_to);
        free((void *) remote_from);
        free((void *) remote_to);
        return renamed;
}
