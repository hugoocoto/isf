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
#include <time.h>
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
        int quiet;                     // don't report what's sent and received
        int lost;                      // the connection broke: said once
        void (*on_local_dir)(Root *root, const char *path);
        int (*on_place)(SyncPlace *places, int n); // the agent puts files in place
        int sent, received, conflicts; // for sync_stats
} g;

/* How far a flush got, for the status line (see sync_progress) */
static struct {
        int on;
        double start, drawn; // when the flush started, and the line was last drawn
        int dirs;            // remote folders listed
        int files, done;     // files to transfer, and transferred
        uint64_t bytes;      // their size
        uint64_t moved;      // sftp_moved when the flush started
} progress;

void
sync_init(Sftp *sftp, const char *host, const char *port, char *const *ssh_opts, int jobs, int dry_run, int quiet)
{
        g.sftp    = sftp;
        g.host    = host;
        g.port    = port;
        g.dry_run = dry_run;
        g.quiet   = quiet;
        g.lost    = 0;
        pool_start(dry_run ? 1 : jobs, host, port, ssh_opts);
}

int
sync_lost(void)
{
        return g.lost || g.sftp->dead;
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
        if (!g.quiet) say(stdout, "  %s %s%s%s%s\n", sent ? "↑" : "↓", show, is_dir ? "/" : "", what ? " " : "", what ? what : "");
        if (sent)
                g.sent++;
        else
                g.received++;
}

static double
now_s(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* N bytes in the unit of TOTAL, as "1.2" or "120" */
static void
in_units(char *buf, size_t size, uint64_t n, uint64_t total, const char **unit)
{
        static const char *units[] = { "bytes", "KB", "MB", "GB", "TB" };
        int u      = 0;
        double div = 1;
        while (total >= 1024 * div && u < 4) {
                div *= 1024;
                u++;
        }
        double v = n / div;
        snprintf(buf, size, u && v < 10 ? "%.1f" : "%.0f", v);
        *unit = units[u];
}

/* Draw the status line, as it is NOW */
static void
progress_draw(double now)
{
        progress.drawn = now;
        char line[160];
        if (progress.files == 0) {
                snprintf(line, sizeof line, "isf: comparing, %d folder%s listed", progress.dirs, progress.dirs == 1 ? "" : "s");
        } else {
                uint64_t moved = __atomic_load_n(&sftp_moved, __ATOMIC_RELAXED) - progress.moved;
                if (moved > progress.bytes) moved = progress.bytes;
                char a[32], b[32];
                const char *unit;
                in_units(a, sizeof a, moved, progress.bytes, &unit);
                in_units(b, sizeof b, progress.bytes, progress.bytes, &unit);
                snprintf(line, sizeof line, "isf: %d of %d file%s, %s of %s %s", progress.done, progress.files,
                         progress.files == 1 ? "" : "s", a, b, unit);
        }
        status_line(line);
}

/* Draw the status line, if the flush has gone on for a while and it wasn't
 * just drawn. Called now and then by the walk and while transfers go. */
static void
progress_tick(void)
{
        if (!progress.on) return;
        double now = now_s();
        if (now - progress.start >= 2 && now - progress.drawn >= 0.5) progress_draw(now);
}

void
sync_progress(int on)
{
        if (on && !progress.on)
                progress = (typeof(progress)) { .on = 1, .start = now_s(), .moved = __atomic_load_n(&sftp_moved, __ATOMIC_RELAXED) };
        else if (!on && progress.on) {
                if (progress.drawn) status_line(NULL);
                progress.on = 0;
        }
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

/* Check an SFTP result: 1 if it worked, else log it and return 0. When the
 * connection breaks that's said once: the rest of the walk fails at once,
 * without changing anything, and main.c connects again. */
static int
sftp_ok(int st, const char *what, const char *remote)
{
        if (st == SFTP_OK) return 1;
        if (st != SFTP_ERR_IO && !g.sftp->dead)
                LOG("Error", "%s '%s:%s': %s", what, g.host, remote, g.sftp->error);
        else if (!g.lost)
                LOG("Error", "%s '%s:%s': %s", what, g.host, remote, g.sftp->error);
        if (st == SFTP_ERR_IO || g.sftp->dead) g.lost = 1;
        return 0;
}

static int
remote_is_dir(Sftp *c, const char *remote)
{
        SftpAttrs a;
        return sftp_lstat(c, remote, &a) == SFTP_OK && S_ISDIR(a.perm);
}

/* Like sftp_ok, but for a transfer on a worker connection (the pool reports a
 * dead connection back to the main thread through c->dead) */
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
 * NUL, the link target (empty if none), NUL. Some builds wrote another number
 * after STAMP: it's skipped. */
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
                if (sscanf(buf.items + pos, "%c %llu %u %o %llu %*u:%n", &type, &size, &mtime, &mode, &stamp, &n) < 5 || n == 0)
                        sscanf(buf.items + pos, "%c %llu %u %o %llu:%n", &type, &size, &mtime, &mode, &stamp, &n);
                if (n == 0) {
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


/* Does the remote PATH still have PLANNED, as far as its attributes tell? If
 * not, it changed meanwhile and isn't replaced: the agent reports the change,
 * and the next round sees it. On C, which may be a worker's connection. */
/* Is what an LSTAT of the remote PATH got (R: its status, A: the attributes)
 * still PLANNED, as far as attributes tell? */
static int
as_planned(const char *path, int r, const SftpAttrs *a, const State *planned)
{
        State now = { 0 };
        if (r == SFTP_OK)
                now = (State) { .type = type_of(a->perm), .size = a->size, .mtime = a->mtime, .mode = a->perm & 07777 };
        else if (r != SFTP_NO_SUCH_FILE)
                return 0; // can't tell: leave it
        int same_now = now.type == planned->type &&
                       (now.type != 'f' || (now.size == planned->size && now.mtime == planned->mtime && now.mode == planned->mode));
        if (!same_now) VPRINT("File: %s [changed on the remote meanwhile, left alone]\n", path);
        return same_now;
}

static int
remote_unchanged(Sftp *c, const char *path, const State *planned)
{
        SftpAttrs a;
        return as_planned(path, sftp_lstat(c, path, &a), &a, planned);
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

/* Paths kept aside while syncing, for sync_take_extra */
static Da(SyncExtra) extra;

/* ROOT's REL was kept next to it as a conflict copy: that copy is new to the
 * other side, so isf syncs it in this same run */
static void
kept_aside(Root *root, const char *rel)
{
        char *copy = malloc(strlen(rel) + sizeof CONFLICT_SUFFIX);
        assert(copy);
        strcpy(stpcpy(copy, rel), CONFLICT_SUFFIX);
        Da_append(&extra, ((SyncExtra) { root, copy }));
}

int
sync_take_extra(SyncExtra **out)
{
        int n  = extra.count;
        *out   = extra.items;
        extra  = (typeof(extra)) { 0 };
        return n;
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
static void expect(Root *root, const char *rel, const State *st);

/* REMOTE, REL in ROOT, is ST: rename it to its conflict name */
static int
keep_remote_copy(Root *root, const char *rel, const char *remote, const State *st)
{
        char *copy = conflict_name(remote);
        int ok     = sftp_ok(sftp_rename(g.sftp, remote, copy), "Cannot keep a copy of", remote);
        if (ok) {
                /* When the agent reports it, it's not someone else's write */
                char *rel_copy = conflict_name(rel);
                expect(root, rel, &(State) { 0 });
                expect(root, rel_copy, st);
                free(rel_copy);
        }
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
 * The bytes of regular files. Each goes to a temp file next to its place and
 * is renamed over it, so it's never seen half written, and only if what it
 * replaces is still what was planned. With a pool they go to the workers,
 * several files at once each, and are recorded and reported when they come
 * back (sync_drain), here on the main thread. */

typedef struct {
        int up;
        Root *root;
        char *rel, *local, *remote, *show;
        State want;   // what the remote has: down, recorded once fetched
        State expect; // what the other side has to still have when replaced
        State result; // what to record, once done
        int ok;
        int place;    // up: written to its temp file there, not in place yet
        int by_hand;  // up: put in place with sftp_rename (it removes the old one first)

        /* While it's under way */
        int fd;       // the local file (up), or the local temp file
        char *parent; // the folder it goes in, on the other side
        char *tmp;    // the temp file there
} Transfer;

/* Get T ready to go, as F; up, ST is the local file as it's sent. Returns 0
 * (logged) if it can't. */
static int
transfer_open(Transfer *t, SftpFile *f, struct stat *st)
{
        t->fd = -1;
        if (t->up) {
                t->fd = open(t->local, O_RDONLY | O_CLOEXEC);
                if (t->fd == -1) {
                        /* ENOENT: removed since, and that has its own event */
                        if (errno != ENOENT) LOG_ERR("Cannot open '%s'", t->local);
                        return 0;
                }
                if (fstat(t->fd, st) == -1 || !S_ISREG(st->st_mode)) return 0;
                t->parent = parent_dir(t->remote);
                t->tmp    = (char *) temp_path(t->parent);
                /* The agent puts it in place, unless there's nobody to ask */
                t->place  = g.on_place != NULL && !g.dry_run;
                *f        = (SftpFile) {
                               .fd           = t->fd,
                               .path         = t->tmp,
                               .target       = t->place ? NULL : t->remote,
                               .attrs        = attrs_of(st),
                               .size         = st->st_size,
                               .expect       = t->expect.type,
                               .expect_attrs = { .size = t->expect.size, .mtime = t->expect.mtime, .perm = t->expect.mode },
                };
                return 1;
        }
        t->parent = parent_dir(t->local);
        if (mkdir_p(t->parent) == -1) {
                LOG_ERR("Cannot create '%s'", t->parent);
                return 0;
        }
        t->fd = temp_create(t->parent, &t->tmp);
        if (t->fd == -1) {
                LOG_ERR("Cannot create a temp file in '%s'", t->parent);
                return 0;
        }
        *f = (SftpFile) { .fd = t->fd, .path = t->remote, .size = t->want.size };
        return 1;
}

/* T went as F says, on C: put it in place, or clean up. Up, ST is what
 * transfer_open found. */
static void
transfer_finish(Sftp *c, Transfer *t, SftpFile *f, const struct stat *st)
{
        int r = f->status;
        char why[sizeof f->error]; // of the last failure, before any cleanup
        snprintf(why, sizeof why, "%s", f->error);
        if (t->up) {
                if (r == SFTP_NO_SUCH_FILE && f->at == SFTP_AT_OPEN) {
                        /* The remote folder is missing: made, and sent again */
                        r = sftp_mkdir_p(c, t->parent);
                        if (r == SFTP_OK && lseek(t->fd, 0, SEEK_SET) == 0) {
                                int io = sftp_put_many(c, f, 1);
                                r      = io ? io : f->status;
                        }
                        snprintf(why, sizeof why, "%s", r == f->status ? f->error : c->error);
                }
                /* Something is in the way of the temp file (one left by a run
                 * that was killed, or worse): other names, a few times */
                for (int i = 0; i < 4 && r != SFTP_OK && r != SFTP_ERR_IO && f->at == SFTP_AT_OPEN; i++) {
                        free(t->tmp);
                        t->tmp  = (char *) temp_path(t->parent);
                        f->path = t->tmp;
                        if (lseek(t->fd, 0, SEEK_SET) != 0) break;
                        int io = sftp_put_many(c, f, 1);
                        r      = io ? io : f->status;
                        snprintf(why, sizeof why, "%s", r == f->status ? f->error : c->error);
                }
                if (r != SFTP_OK && r != SFTP_ERR_IO && f->at == SFTP_AT_RENAME) {
                        /* Written, not in place: a server that can't replace
                         * in one step, or a directory in the way */
                        t->by_hand = 1;
                        r          = sftp_rename(c, t->tmp, t->remote);
                        snprintf(why, sizeof why, "%s", c->error);
                        if (r != SFTP_OK && r != SFTP_ERR_IO && remote_is_dir(c, t->remote)) {
                                conn_ok(c, sftp_remove_all(c, t->remote), "Cannot remove", t->remote);
                                r = sftp_rename(c, t->tmp, t->remote);
                                snprintf(why, sizeof why, "%s", c->error);
                        }
                        if (r != SFTP_OK) sftp_remove(c, t->tmp);
                }
                t->ok = r == SFTP_OK;
                if (t->ok)
                        t->result = (State) {
                                .type  = 'f',
                                .size  = st->st_size,
                                .mtime = st->st_mtime,
                                .mode  = st->st_mode & 07777,
                                .stamp = ctime_ns(st),
                        };
        } else {
                t->ok = r == SFTP_OK;
                /* The same mode and mtime as the remote, so both look the same */
                struct timespec times[2] = { { .tv_nsec = UTIME_OMIT }, { .tv_sec = t->want.mtime } };
                if (t->ok && (fchmod(t->fd, t->want.mode) == -1 || futimens(t->fd, times) == -1)) {
                        LOG_ERR("Cannot set the mode or mtime of '%s'", t->tmp);
                        t->ok = 0;
                }
                if (t->ok && !local_unchanged(t->local, &t->expect)) t->ok = 0;
                if (t->ok) {
                        struct stat st;
                        if (lstat(t->local, &st) == 0 && S_ISDIR(st.st_mode)) local_remove_all(t->local); // the file replaced it
                        if (rename(t->tmp, t->local) == -1) {
                                LOG_ERR("Cannot rename to '%s'", t->local);
                                t->ok = 0;
                        }
                }
                if (!t->ok) unlink(t->tmp);
                if (t->ok) t->result = fetched(t->local, &t->want);
        }
        if (r == SFTP_CHANGED)
                VPRINT("File: %s [changed on the remote meanwhile, left alone]\n", t->remote);
        else if (r != SFTP_OK && r != SFTP_ERR_IO)
                LOG("Error", "Cannot %s '%s:%s': %s", t->up ? "upload" : "download", g.host, t->remote, why);
        else if (r == SFTP_ERR_IO && !c->dead)
                LOG("Error", "Cannot %s '%s:%s': %s", t->up ? "upload" : "download", g.host, t->remote, c->error);
}

/* Send or fetch the transfers ARGS, on C, all at once */
static void
transfer_run(Sftp *c, void **args, int n)
{
        Transfer *up[POOL_BATCH], *down[POOL_BATCH];
        SftpFile upf[POOL_BATCH], downf[POOL_BATCH];
        struct stat upst[POOL_BATCH];
        int nup = 0, ndown = 0;
        int dead = c->dead; // then it can't, and that was said

        for (int i = 0; i < n; i++) {
                Transfer *t = args[i];
                if (dead) continue;
                if (t->up && transfer_open(t, &upf[nup], &upst[nup]))
                        up[nup++] = t;
                else if (!t->up && transfer_open(t, &downf[ndown], NULL))
                        down[ndown++] = t;
        }
        if (nup) {
                if (sftp_put_many(c, upf, nup) == SFTP_ERR_IO && !dead) LOG("Error", "Cannot upload to '%s': %s", g.host, c->error);
                for (int i = 0; i < nup; i++)
                        transfer_finish(c, up[i], &upf[i], &upst[i]);
        }
        if (ndown) {
                if (sftp_get_many(c, downf, ndown) == SFTP_ERR_IO && !c->dead) LOG("Error", "Cannot download from '%s': %s", g.host, c->error);
                for (int i = 0; i < ndown; i++)
                        transfer_finish(c, down[i], &downf[i], NULL);
        }
        for (int i = 0; i < n; i++) {
                Transfer *t = args[i];
                if (t->fd != -1) close(t->fd);
                t->fd = -1;
        }
}

/* isf just put ST at REL there: what it saw of that path before (the agent's
 * messages about isf's own doing, read while transfers went on) is out of
 * date, and the plan would go on believing it until the next flush. */
static void
seen_now(Root *root, const char *rel, const State *st)
{
        State *s = bt_get(&root->seen, rel);
        if (s == NULL) {
                s = calloc(1, sizeof *s);
                assert(s);
                bt_add(&root->seen, rel, s);
        }
        *s         = *st;
        s->link    = NULL;
        s->written = 0; // isf's own write, not someone else's
}

/* Transfers written to their temp file on the remote, waiting to be put in
 * place (see place_pending) */
static Da(Transfer *) placing;

/* Record and report a transfer that's done, and free it */
static void
transfer_done(void *arg)
{
        Transfer *t = arg;
        if (t->ok && t->place) { // sync_drain has it put in place first
                Da_append(&placing, t);
                return;
        }
        /* Counted before it's reported: the report redraws the line under it */
        progress.done++;
        if (progress.on && progress.drawn && t->ok && !g.quiet) progress_draw(now_s());
        if (t->ok) {
                /* Put in place by hand: the old one was removed first, and
                 * the rename may be a link and an unlink. When the agent
                 * reports those, they're not someone else's doing. */
                if (t->by_hand) {
                        expect(t->root, t->rel, &(State) { 0 });
                        expect(t->root, t->rel, &t->result);
                }
                record_set(t->root, t->rel, &t->result);
                if (t->up) seen_now(t->root, t->rel, &t->result);
                report(t->up, t->show, 0, NULL);
        }
        progress_tick();
        state_free(&t->want);
        state_free(&t->expect);
        state_free(&t->result);
        free(t->rel);
        free(t->local);
        free(t->remote);
        free(t->show);
        free(t->parent);
        free(t->tmp);
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
 * folder or a wiped disk: syncing would delete everything on the other side.
 * The remote listing it takes is kept for the next walk (root->seed). */
static int
looks_wiped(Root *root)
{
        if (!root->seed.listed) {
                /* The agent didn't: here */
                SftpDir have = { 0 };
                SftpAttrs self;
                int found = 0;
                int r     = sftp_readdir_self(g.sftp, root->remote, &have, &self, &found);
                sftp_ok(r == SFTP_NO_SUCH_FILE ? SFTP_OK : r, "Cannot list", root->remote);
                if (r == SFTP_OK || r == SFTP_NO_SUCH_FILE) {
                        root->seed.listed    = 1;
                        root->seed.status    = r;
                        root->seed.dir       = have;
                        root->seed.has_state = found || r == SFTP_NO_SUCH_FILE;
                        if (found) root->seed.state = remote_state_from(root->remote, &self);
                } else {
                        sftp_dir_free(&have);
                }
        }
        SftpDir *have = &root->seed.dir;
        if (have->count) qsort(have->items, have->count, sizeof *have->items, entry_cmp);
        int remote_empty = root->seed.listed && have->count == 0;
        if (root->rec.count <= 1) return 0; // only the folder itself

        int local_empty = 1;
        DIR *dir        = opendir(root->local);
        if (dir) {
                struct dirent *e;
                while (local_empty && (e = readdir(dir)))
                        local_empty = !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..");
                closedir(dir);
        }
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

static BT ahead; // remote path -> Listing *

static Listing *
listing(const char *remote)
{
        Listing *l = bt_get(&ahead, remote);
        if (l) return l;
        l = calloc(1, sizeof *l);
        assert(l);
        bt_add(&ahead, remote, l);
        return l;
}

static void
ahead_forget(void)
{
        BT *n;
        for_bt_each(n, &ahead)
        {
                Listing *l = n->value;
                sftp_dir_free(&l->dir);
                free(l);
        }
        bt_destroy(&ahead);
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

/* Add to NEXT and NEXT_RELS the subdirectories of the listing DIR of REMOTE
 * (REL in ROOT) that children() would go into */
static void
queue_subdirs(Root *root, const char *remote, const char *rel, const SftpDir *dir, Names *next, Names *next_rels)
{
        Da_foreach(e, *dir)
        {
                if (!S_ISDIR(e->attrs.perm) || !name_safe(e->name) || is_temp_name(e->name)) continue;
                const char *child  = *rel ? pathjoin(rel, e->name) : strdup(e->name);
                const char *path   = pathjoin(remote, e->name);
                const Listing *had = bt_get(&ahead, path);
                if (ignored(&root->ign, child, 1) || (had && had->listed && !had->taken)) { // or the agent listed it
                        free((void *) child);
                        free((void *) path);
                        continue;
                }
                Da_append(next, (char *) path);
                Da_append(next_rels, (char *) child);
        }
}

/* Put the listings of the folders below ROOT the agent sent (seed.below) in
 * ahead. A folder in one of them it didn't list (past its limit, or ignored
 * there) is listed when the walk gets there, not taken for an empty one. */
static void
seed_below(Root *root)
{
        BT *n;
        for_bt_each(n, &root->seed.below)
        {
                SftpDir *dir     = n->value;
                const char *path = root_join(root->remote, n->key);
                Listing *l       = listing(path);
                sftp_dir_free(&l->dir);
                if (dir->count) qsort(dir->items, dir->count, sizeof *dir->items, entry_cmp);
                *l = (Listing) { .listed = 1, .status = SFTP_OK, .dir = *dir };
                free(dir);
                n->value = NULL;
                Da_foreach(e, l->dir)
                {
                        if (!S_ISDIR(e->attrs.perm) || !name_safe(e->name)) continue;
                        const char *child = pathjoin(path, e->name);
                        if (!bt_get(&ahead, child)) listing(child); // not listed yet
                        free((void *) child);
                }
                free((void *) path);
        }
        bt_destroy(&root->seed.below);
}

/* List the directories LEVEL (remote paths; RELS: in ROOT) and the ones below
 * them (up to READAHEAD_MAX entries) into ahead, a level at a time. What
 * children() would skip isn't listed. Frees LEVEL and RELS. */
static void
read_levels(Root *root, Names level, Names rels)
{
        int entries = 0;
        while (level.count > 0) {
                SftpDir *out = calloc(level.count, sizeof *out);
                int *status  = calloc(level.count, sizeof *status);
                assert(out && status);
                if (!sftp_ok(sftp_readdir_many(g.sftp, level.items, level.count, out, status), "Cannot list", root->remote)) {
                        /* Broken off: what wasn't listed isn't empty, it's unknown */
                        for (int i = 0; i < level.count; i++)
                                if (status[i] == SFTP_OK) status[i] = SFTP_NO_CONNECTION;
                }
                progress.dirs += level.count;

                Names next = { 0 }, next_rels = { 0 };
                for (int i = 0; i < level.count; i++) {
                        Listing *l = listing(level.items[i]);
                        sftp_dir_free(&l->dir);
                        *l = (Listing) { .listed = 1, .status = status[i], .dir = out[i] };
                        entries += l->dir.count;
                        if (l->dir.count) qsort(l->dir.items, l->dir.count, sizeof *l->dir.items, entry_cmp);
                        queue_subdirs(root, level.items[i], rels.items[i], &l->dir, &next, &next_rels);
                }
                free(out);
                free(status);
                names_free(&level);
                names_free(&rels);
                level = next;
                rels  = next_rels;
                progress_tick();

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

/* List REMOTE, which is REL inside ROOT, and the directories below it */
static void
read_ahead(Root *root, const char *rel, const char *remote)
{
        Names level = { 0 }, rels = { 0 };
        Da_append(&level, strdup(remote));
        Da_append(&rels, strdup(rel));
        read_levels(root, level, rels);
}

/* Put the remote listing of REMOTE, which is REL inside ROOT, sorted by name,
 * in *DIR (free it with sftp_dir_free). Empty if REMOTE is missing. Returns
 * SFTP_OK, or why it couldn't be listed. */
static int
take_listing(Root *root, const char *rel, const char *remote, SftpDir *dir)
{
        *dir       = (SftpDir) { 0 };
        Listing *l = bt_get(&ahead, remote);
        if (l == NULL) {
                /* Its parent was listed without it as a directory: it's
                 * missing on the remote, or was just made there, empty. Its
                 * own children are then missing too: it counts as listed. */
                char *parent = parent_dir(remote);
                Listing *p   = bt_get(&ahead, parent);
                free(parent);
                if (p && p->listed) {
                        l = listing(remote);
                        *l = (Listing) { .listed = 1, .status = SFTP_OK, .taken = 1 };
                        return SFTP_OK;
                }
        }
        if (l == NULL || !l->listed || l->taken) {
                read_ahead(root, rel, remote);
                l = bt_get(&ahead, remote);
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
        Plan plan;         // the steps not done yet, and some done
        int done;          // steps done so far, in PLAN
        Names failed;      // paths whose steps failed
        Da(int) batch;     // steps waiting to be done together (batch_flush)
        int counted;       // steps in PLAN counted in KEEPS so far,
        int keeps[2];      // and how many of them leave something here (0)
                           // or there (1), for plan_keeps
} Walk;

static int plan_path(Walk *w, const char *rel, int what, const State *remote_now);
static void apply_ready(Walk *w);
static void batch_flush(Walk *w);

/* A temp file older than this is a leftover of an interrupted transfer: one
 * in use is written to all the time */
#define TEMP_MAX_AGE (24 * 3600)

/* NAME, inside REL, is one of isf's temp files: on the remote as E (or NULL),
 * here in LOCAL (the folder). If it's a leftover, on either side, plan to
 * remove it. */
static void
plan_leftover(Walk *w, const char *rel, const char *name, const SftpEntry *e, const char *local)
{
        time_t old        = time(NULL) - TEMP_MAX_AGE;
        const char *child = *rel ? pathjoin(rel, name) : strdup(name);
        const char *path  = pathjoin(local, name);
        if (e && S_ISREG(e->attrs.perm) && e->attrs.mtime < old) {
                State M = { .type = 'f', .size = e->attrs.size, .mtime = e->attrs.mtime, .mode = e->attrs.perm & 07777 };
                plan_add(&w->plan, ACT_REMOVE, 1, child, NULL, &M, NULL);
                w->plan.items[w->plan.count - 1].quiet = 1;
                VPRINT("File: %s:%s [left by an interrupted transfer, removed]\n", g.host, child);
        }
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_mtime < old) {
                State L = local_state(path);
                plan_add(&w->plan, ACT_REMOVE, 0, child, &L, NULL, NULL);
                w->plan.items[w->plan.count - 1].quiet = 1;
                state_free(&L);
                VPRINT("File: %s [left by an interrupted transfer, removed]\n", path);
        }
        free((void *) child);
        free((void *) path);
}

/* Plan everything directly inside REL, and below: what either side has, and
 * what the record has (gone from one side or both since) */
static int
plan_children(Walk *w, const char *rel, const State *M)
{
        Root *root         = w->root;
        int all            = 0; // everything in it could be looked at
        const char *local  = root_join(root->local, rel);
        const char *remote = root_join(root->remote, rel);
        Names names        = { 0 };
        SftpDir have;

        progress_tick();

        /* Without the remote listing everything there would look deleted */
        if (!sftp_ok(take_listing(root, rel, remote, &have), "Cannot list", remote)) goto out;
        /* A folder there that can be read but not looked into (no x for isf)
         * lists as empty instead of failing, and everything here would be
         * deleted to match it. Only its mode tells the two apart, and syncing
         * that mode is what makes it readable again. */
        if (have.count == 0 && M && M->type == 'd' && !(M->mode & 0100)) {
                LOG("Error", "Cannot look inside '%s:%s' (its mode is %o there): leaving what's in it alone",
                    g.host, remote, M->mode);
                goto out;
        }
        all = 1;
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
                if (is_temp_name(name)) {
                        plan_leftover(w, rel, name, find_entry(&have, name), local);
                        continue;
                }
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
        return all;
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
        /* The remote ones all at once */
        Da(SftpOp) ops = { 0 };
        Da_foreach(d, dir_modes)
        {
                if (!d->up) {
                        if (chmod(d->path, d->mode) == -1) LOG_ERR("Cannot set the mode of '%s'", d->path);
                        continue;
                }
                SftpOp o = { .op = SFTP_OP_SETSTAT, .path = d->path };
                o.attrs  = (SftpAttrs) { .flags = SFTP_ATTR_PERMISSIONS, .perm = d->mode };
                Da_append(&ops, o);
        }
        if (ops.count && sftp_ok(sftp_batch(g.sftp, ops.items, ops.count), "Cannot set modes in", g.host)) {
                Da_foreach(o, ops)
                {
                        if (o->status != SFTP_OK)
                                LOG("Error", "Cannot set the mode of '%s:%s': %s", g.host, o->path, o->error);
                }
        }
        Da_destroy(&ops);
        Da_foreach(d, dir_modes)
        {
                free(d->path);
        }
        dir_modes.count = 0;
}

/* Count the steps planned since the last time, for plan_keeps */
static void
count_keeps(Walk *w)
{
        for (; w->counted < w->plan.count; w->counted++) {
                const Action *a = &w->plan.items[w->counted];
                for (int up = 0; up < 2; up++)
                        if (a->type == ACT_RECORD ? a->st.type != 0 :
                                                    a->up == up && (a->type == ACT_COPY || a->type == ACT_MKDIR || a->type == ACT_MODE))
                                w->keeps[up]++;
        }
}

/* How many steps planned so far leave something on the side UP names: if it
 * grew, a step planned since leaves something */
static int
plan_keeps(Walk *w, int up)
{
        count_keeps(w);
        return w->keeps[up];
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
                int all    = !(deep || is_new) || plan_children(w, rel, M);
                /* Last: a read-only mode would stop the steps inside */
                State st = *L;
                uint32_t mode;
                int up;
                if (dir_mode(L, M, R, &mode, &up)) {
                        st.mode = mode;
                        plan_add(plan, ACT_MODE, up, rel, L, M, &st);
                }
                plan_add(plan, ACT_RECORD, 0, rel, L, M, &st);
                return all && (deep || is_new);
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
                int kept = plan_keeps(w, ld);
                int all  = plan_children(w, rel, M);
                if (!all) {
                        /* What's inside couldn't be looked at: both sides
                         * stay as they are, and the error is said above */
                        plan_add(plan, ACT_RECORD, 0, rel, L, M, D);
                } else if (plan_keeps(w, ld) > kept) {
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
                if (!ld) {
                        /* Made here first: what it replaces may be a symlink
                         * to somewhere else, and what's inside would be read
                         * through it (that's another folder's, not this one's) */
                        apply_ready(w);
                        batch_flush(w);
                }
                plan_children(w, rel, M);
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
        const State *seen  = bt_get(&root->seen, rel);
        Record *rec        = record_find(root, rel);
        /* What the agent just saw of it, if it told (not a symlink's target):
         * no need to ask the remote again */
        State M = remote_now           ? state_copy(remote_now) :
                  seen && seen->type != 'l' ? state_copy(seen) :
                                              remote_state(remote);
        State R            = rec ? state_copy(&rec->st) : (State) { 0 };
        int covered        = 0;

        if (M.type == 'f' && seen) M.written = seen->written;

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

/* Make the missing folders LOCAL goes in, here on the main thread, and watch
 * each at once (like a MKDIR step's): a download would make them on a worker,
 * and their events would then look into them again */
static void
local_parents(Root *root, const char *local)
{
        char *parent = parent_dir(local);
        struct stat st;
        if (lstat(parent, &st) == -1 && errno == ENOENT) {
                local_parents(root, parent);
                if (mkdir(parent, 0777) == 0 && g.on_local_dir) g.on_local_dir(root, parent);
        }
        free(parent);
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
                        if (keep)
                                say(stdout, "  ! %s changed on both sides: kept the newest, the other one is %s%s\n", show, show, CONFLICT_SUFFIX);
                        else
                                say(stdout, "  ! %s changed on both sides: kept the newest\n", show);
                        g.conflicts++;
                }
                if (g.dry_run) {
                        report(a->up, show, 0, NULL);
                        break;
                }
                /* What it replaces has to still be what was planned: checked
                 * here, or for a file sent there, just before it takes its
                 * place (send_file) */
                if (!a->up)
                        ok = local_unchanged(local, &a->L);
                else if (keep || W->type != 'f')
                        ok = remote_unchanged(g.sftp, remote, &a->M);
                if (!ok) break;
                if (keep) {
                        if (!(ok = a->up ? keep_remote_copy(root, a->rel, remote, O) : keep_local_copy(local))) break;
                        kept_aside(root, a->rel);
                }
                const State *expect = keep ? &none : O; // what the other side has now
                if (!a->up) local_parents(root, local);

                if (W->type == 'f') {
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
                                .fd     = -1,
                        };
                        progress.files++;
                        progress.bytes += a->up ? a->L.size : a->M.size;
                        if (pool_enabled()) {
                                pool_submit(transfer_run, t);
                        } else {
                                transfer_run(g.sftp, (void *[]) { t }, 1);
                                ok = t->ok;
                                transfer_done(t);
                        }
                        break;
                }
                /* A symlink */
                State done = { 0 };
                if (a->up) {
                        ok = send_symlink(local, remote, &done);
                } else {
                        ok = fetch_symlink(local, &a->M);
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
                        ok = remote_unchanged(g.sftp, remote, &a->M) &&
                             sftp_ok(sftp_setstat(g.sftp, remote, &at), "Cannot set the mode of", remote);
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
                        if ((a->up ? &a->M : &a->L)->type && !a->quiet) report(a->up, show, 0, "deleted");
                        break;
                }
                if (a->up) {
                        if (!(ok = remote_unchanged(g.sftp, remote, &a->M))) break;
                        int r = sftp_remove_all(g.sftp, remote);
                        if (r == SFTP_OK) report(1, show, 0, "deleted");
                        ok = r == SFTP_NO_SUCH_FILE || sftp_ok(r, "Cannot remove", remote);
                } else if ((ok = local_unchanged(local, &a->L))) {
                        local_remove_all(local);
                        if (a->L.type && !a->quiet) report(0, show, 0, "deleted");
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
                        if (!(ok = a->up ? remote_unchanged(g.sftp, remote, &a->M) : local_unchanged(local, &a->L))) break;
                        if (a->keep) {
                                ok = a->up ? keep_remote_copy(root, a->rel, remote, O) : keep_local_copy(local);
                                if (ok) kept_aside(root, a->rel);
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
                if (ok && !a->up && g.on_local_dir) g.on_local_dir(root, local);
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
                        if (r == SFTP_ERR_IO) sftp_ok(r, "Cannot remove", remote);
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

/* Did a step before fail for PATH, or a folder it's in? */
static int
failed_for(const Walk *w, const char *path)
{
        Da_foreach(f, w->failed)
        {
                if (!strcmp(path, *f) || inside(path, *f)) return 1;
        }
        return 0;
}

/* Is step A one whose remote request goes with the batch's? */
static int
batched(const Action *a)
{
        return a->up && (a->type == ACT_REMOVE || a->type == ACT_RMDIR || (a->type == ACT_MKDIR && a->M.type == 0));
}

/* Do the steps waiting in W->batch. The server does requests in order, so
 * the remote changes of a whole run of steps go together, in the order they
 * were planned: a folder's MKDIR before those inside it, the removals inside
 * a folder before its RMDIR. Rounds:
 *   1. the check of each file to remove, LSTAT (it has to be what was planned)
 *   2. every MKDIR, the REMOVEs of what passed, every RMDIR
 *   3. (rarely) the MKDIRs that failed again, one by one, with sftp_mkdir_p:
 *      there already, or their parent went missing
 * Then, in order, the results and the steps that waited (records, local
 * steps, transfers: only now, when their folder is there). */
static void
batch_flush(Walk *w)
{
        int n = w->batch.count;
        if (n == 0) return;
        Root *root   = w->root;
        char **paths = calloc(n, sizeof *paths), **aside = calloc(n, sizeof *aside);
        SftpOp *ck = calloc(2 * n, sizeof *ck), *op = calloc(n, sizeof *op);
        int *check = calloc(n, sizeof *check), *req = calloc(n, sizeof *req); // index in CK/OP, or -1
        assert(paths && aside && ck && op && check && req);
        int nck = 0, nop = 0;

        /* A file to remove is first moved aside, to a temp name next to it,
         * and checked there. Someone writing it after that makes a new file,
         * which stays; if it changed before, it's moved back. */
        for (int i = 0; i < n; i++) {
                const Action *a = &w->plan.items[w->batch.items[i]];
                check[i] = req[i] = -1;
                if (!batched(a)) continue;
                paths[i] = (char *) root_join(root->remote, a->rel);
                if (a->type == ACT_REMOVE) {
                        char *parent = parent_dir(paths[i]);
                        aside[i]     = (char *) temp_path(parent);
                        free(parent);
                        check[i]  = nck;
                        ck[nck++] = (SftpOp) { .op = SFTP_OP_RENAME, .path = paths[i], .to = aside[i], .status = SFTP_ERR_IO };
                        ck[nck++] = (SftpOp) { .op = SFTP_OP_LSTAT, .path = aside[i], .status = SFTP_ERR_IO };
                }
        }
        int io = nck ? sftp_ok(sftp_batch(g.sftp, ck, nck), "Cannot check", root->remote) : 1;

        for (int i = 0; io && i < n; i++) {
                const Action *a = &w->plan.items[w->batch.items[i]];
                if (!batched(a)) continue;
                int kind         = a->type == ACT_MKDIR ? SFTP_OP_MKDIR : a->type == ACT_RMDIR ? SFTP_OP_RMDIR : SFTP_OP_REMOVE;
                const char *path = paths[i];
                if (check[i] >= 0) {
                        SftpOp *mv = &ck[check[i]], *c = &ck[check[i] + 1];
                        if (mv->status != SFTP_OK) continue; // gone already, or it can't be (said below)
                        if (c->status == SFTP_NO_SUCH_FILE) {
                                mv->status = SFTP_NO_SUCH_FILE; // and gone from there too
                                continue;
                        }
                        path = aside[i];
                        if (!as_planned(paths[i], c->status, &c->attrs, &a->M)) kind = SFTP_OP_RENAME;
                }
                req[i]    = nop;
                op[nop++] = (SftpOp) { .op = kind, .path = path, .to = paths[i], .status = SFTP_ERR_IO };
        }
        if (io && nop) io = sftp_ok(sftp_batch(g.sftp, op, nop), "Cannot change", root->remote);
        for (int i = 0; io && i < n; i++) {
                SftpOp *o = req[i] >= 0 ? &op[req[i]] : NULL;
                if (o && o->op == SFTP_OP_MKDIR && o->status != SFTP_OK) {
                        o->status = sftp_mkdir_p(g.sftp, o->path);
                        if (o->status != SFTP_OK) snprintf(o->error, sizeof o->error, "%s", g.sftp->error);
                }
                if (o && o->op == SFTP_OP_RENAME && o->status != SFTP_OK && o->status != SFTP_ERR_IO) {
                        /* A new one took its place meanwhile: kept next to it */
                        char *keep = malloc(strlen(paths[i]) + sizeof CONFLICT_SUFFIX);
                        assert(keep);
                        strcat(strcpy(keep, paths[i]), CONFLICT_SUFFIX);
                        if (sftp_rename(g.sftp, aside[i], keep) == SFTP_OK)
                                kept_aside(root, w->plan.items[w->batch.items[i]].rel);
                        else
                                LOG("Error", "Cannot put '%s:%s' back, it's in '%s': %s", g.host, paths[i], aside[i], g.sftp->error);
                        free(keep);
                }
        }

        for (int i = 0; i < n; i++) {
                const Action *a = &w->plan.items[w->batch.items[i]];
                if (failed_for(w, a->rel)) continue;
                if (!batched(a)) {
                        if (!apply_step(root, a)) Da_append(&w->failed, strdup(a->rel));
                        continue;
                }
                /* How it went: the move aside, then the request (none if it
                 * was gone already) */
                SftpOp *c = check[i] >= 0 ? &ck[check[i]] : NULL;
                SftpOp *o = req[i] >= 0 ? &op[req[i]] : NULL;
                int st    = !io ? SFTP_ERR_IO : o ? o->status : c ? c->status : SFTP_ERR_IO;
                if (io && o && o->op == SFTP_OP_RENAME) st = SFTP_CHANGED; // moved back: it stays
                const char *why = o ? o->error : c ? c->error : "";
                char *show      = show_path(root, a->rel);
                if (a->type == ACT_RMDIR) {
                        if (st == SFTP_OK) report(1, show, 1, "deleted");
                        /* Else something inside couldn't be synced: it stays */
                        record_set(root, a->rel, st == SFTP_OK ? &(State) { 0 } : &a->st);
                } else if (st == SFTP_OK || (a->type == ACT_REMOVE && st == SFTP_NO_SUCH_FILE)) {
                        if (a->type == ACT_MKDIR)
                                report(1, show, 1, NULL);
                        else {
                                if (st == SFTP_OK && !a->quiet) report(1, show, 0, "deleted");
                                record_set(root, a->rel, &(State) { 0 });
                        }
                } else {
                        if (st != SFTP_CHANGED && st != SFTP_ERR_IO)
                                LOG("Error", "Cannot %s '%s:%s': %s", a->type == ACT_MKDIR ? "create" : "remove",
                                    g.host, paths[i], why);
                        Da_append(&w->failed, strdup(a->rel));
                }
                free(show);
        }

        for (int i = 0; i < n; i++) {
                free(paths[i]);
                free(aside[i]);
        }
        free(paths);
        free(aside);
        free(ck);
        free(op);
        free(check);
        free(req);
        w->batch.count = 0;
}

/* Do the steps planned since the last time, in order, in batches (see
 * batch_flush). When one fails, the steps for the same path and what's inside
 * it are skipped: they need it. A MKDIR with something in the way doesn't
 * wait: what's inside it needs it done before its own MKDIRs go. */
static void
apply_ready(Walk *w)
{
        for (; w->done < w->plan.count; w->done++) {
                const Action *a = &w->plan.items[w->done];
                if (!g.dry_run && !(a->type == ACT_MKDIR && a->up && a->M.type != 0)) {
                        Da_append(&w->batch, w->done);
                        if (w->batch.count >= 256) batch_flush(w);
                        continue;
                }
                batch_flush(w);
                if (failed_for(w, a->rel)) continue;
                if (!apply_step(w->root, a)) Da_append(&w->failed, strdup(a->rel));
        }

        /* Forget the steps done (and counted), so a walk of a big tree doesn't
         * keep them all: up to the first one still in the batch */
        int drop = w->batch.count ? w->batch.items[0] : w->done;
        if (drop < 1024) return;
        count_keeps(w);
        for (int i = 0; i < drop; i++) {
                Action *a = &w->plan.items[i];
                free(a->rel);
                state_free(&a->L);
                state_free(&a->M);
                state_free(&a->st);
        }
        memmove(w->plan.items, w->plan.items + drop, (w->plan.count - drop) * sizeof *w->plan.items);
        w->plan.count -= drop;
        w->done -= drop;
        w->counted -= drop;
        Da_foreach(i, w->batch)
        {
                *i -= drop;
        }
}

int
reconcile(Root *root, const char *rel, int what, const State *remote_now)
{
        /* Something directly in the remote folder, listed already
         * (sync_check): what's there needn't be asked again */
        State seeded = { 0 };
        if (*rel && !strchr(rel, '/') && root->seed.listed && !remote_now && !bt_get(&root->seen, rel)) {
                const SftpEntry *e = find_entry(&root->seed.dir, rel);
                if (!e || !S_ISLNK(e->attrs.perm)) {
                        if (e) {
                                const char *remote = pathjoin(root->remote, rel);
                                seeded             = remote_state_from(remote, &e->attrs);
                                free((void *) remote);
                        }
                        remote_now = &seeded;
                }
        }
        /* All of it, with the remote folder listed already (the agent, or
         * sync_check) */
        if (*rel == 0 && root->seed.listed) {
                Listing *l = listing(root->remote);
                sftp_dir_free(&l->dir);
                *l = (Listing) { .listed = 1, .status = root->seed.status, .dir = root->seed.dir };
                root->seed.dir    = (SftpDir) { 0 };
                root->seed.listed = 0;
                if (!remote_now && root->seed.has_state) remote_now = &root->seed.state;
                root->seed.has_state = 0;
                /* And what's below, from what the agent listed, then over
                 * SFTP as read_ahead would */
                seed_below(root);
                Names level = { 0 }, rels = { 0 };
                queue_subdirs(root, root->remote, "", &l->dir, &level, &rels);
                read_levels(root, level, rels);
        }
        Walk w      = { .root = root };
        int covered = plan_path(&w, rel, what, remote_now);
        apply_ready(&w);
        batch_flush(&w);
        ahead_forget(); // the listings read ahead were for this walk
        plan_free(&w.plan);
        names_free(&w.failed);
        Da_destroy(&w.batch);
        state_free(&seeded);
        return covered;
}

/* What isf just did there itself, besides writing files through its temp
 * files (the agent tells those apart), until the agent reports it: not
 * someone else's write. A server without posix-rename renames with link and
 * unlink, which the agent sees as a file removed and one made. */
typedef struct {
        State st[2]; // what it made of the path, in turn (type 0: removed it)
        int count;
        time_t when; // forgotten a minute later, reported or not
} Expect;

static void
expect(Root *root, const char *rel, const State *st)
{
        Expect *e = bt_get(&root->expect, rel);
        if (e == NULL) {
                e = calloc(1, sizeof *e);
                assert(e);
                bt_add(&root->expect, rel, e);
        }
        if (e->count == 2) e->count = 1; // the oldest goes
        e->st[e->count]      = *st;
        e->st[e->count].link = NULL;
        e->count++;
        e->when = time(NULL);
}

void
sync_remote_seen(Root *root, const char *rel, const State *seen, char kind)
{
        State *s = bt_get(&root->seen, rel);
        if (s == NULL) {
                s = calloc(1, sizeof *s);
                assert(s);
                bt_add(&root->seen, rel, s);
        }
        int written = s->written;
        *s          = *seen;
        s->link     = NULL;
        s->written  = written;
        if (kind != 'C') return; // isf's own write, or only attributes

        /* A write: someone else's, unless it's what isf just did there */
        Expect *e = bt_get(&root->expect, rel);
        for (int i = 0; e && i < e->count; i++) {
                const State *x = &e->st[i];
                if (x->type != seen->type ||
                    (x->type == 'f' && (x->size != seen->size || x->mtime != seen->mtime || x->mode != seen->mode)))
                        continue;
                e->st[i] = e->st[--e->count];
                if (e->count == 0) {
                        bt_del(&root->expect, rel);
                        free(e);
                }
                return;
        }
        s->written = 1;
}

void
sync_forget_seen(Root *root)
{
        BT *n;
        for_bt_each(n, &root->seen)
        {
                free(n->value);
        }
        bt_destroy(&root->seen);

        /* Expectations a minute old weren't reported: they won't be */
        time_t old = time(NULL) - 60;
        Names stale = { 0 };
        for_bt_each(n, &root->expect)
        {
                if (((Expect *) n->value)->when < old) Da_append(&stale, strdup(n->key));
        }
        Da_foreach(k, stale)
        {
                free(bt_get(&root->expect, *k));
                bt_del(&root->expect, *k);
        }
        names_free(&stale);
}

int
sync_unchanged(Root *root, const char *rel)
{
        Record *r         = record_find(root, rel);
        const char *local = root_join(root->local, rel);
        State L           = local_state(local);
        int same_now      = r ? same_local(&L, &r->st) : L.type == 0; // gone, and not synced
        state_free(&L);
        free((void *) local);
        return same_now;
}

void
sync_forget_seed(Root *root)
{
        sftp_dir_free(&root->seed.dir);
        state_free(&root->seed.state);
        BT *n;
        for_bt_each(n, &root->seed.below)
        {
                sftp_dir_free(n->value);
                free(n->value);
        }
        bt_destroy(&root->seed.below);
        root->seed = (typeof(root->seed)) { 0 };
}

void
sync_remote_listed(Root *root, const char *rel, const SftpAttrs *self, SftpDir *dir)
{
        if (dir == NULL || root->seed.broken) {
                if (dir) sftp_dir_free(dir);
                if (!root->seed.broken) LOG_WARN("A damaged listing from the agent: listing '%s' here instead", root->remote);
                sync_forget_seed(root);
                root->seed.broken = 1;
                return;
        }
        SftpDir *to = &root->seed.dir;
        if (*rel == 0 && !root->seed.listed) {
                root->seed.listed    = 1;
                root->seed.status    = SFTP_OK;
                root->seed.has_state = 1;
                root->seed.state     = remote_state_from(root->remote, self);
        } else if (*rel) {
                to = bt_get(&root->seed.below, rel);
                if (to == NULL) {
                        to = calloc(1, sizeof *to);
                        assert(to);
                        bt_add(&root->seed.below, rel, to);
                }
        }
        Da_foreach(e, *dir)
        {
                Da_append(to, *e); // its name goes with it
        }
        Da_destroy(dir);
}

void
sync_on_local_dir(void (*fn)(Root *root, const char *path))
{
        g.on_local_dir = fn;
}

void
sync_on_place(int (*fn)(SyncPlace *places, int n))
{
        g.on_place = fn;
}

/* The agent couldn't put T in place: do it over SFTP, as isf did before there
 * was an agent to ask. Returns 'o', 'c' (it changed there) or 'e'. */
static char
place_over_sftp(Transfer *t)
{
        if (g.sftp->dead) return 'e';
        if (!remote_unchanged(g.sftp, t->remote, &t->expect)) {
                sftp_remove(g.sftp, t->tmp);
                return 'c';
        }
        /* The rename is isf's own doing: not someone else's write */
        expect(t->root, t->rel, &(State) { 0 });
        expect(t->root, t->rel, &t->result);
        int r = sftp_rename(g.sftp, t->tmp, t->remote);
        if (r != SFTP_OK && r != SFTP_ERR_IO && remote_is_dir(g.sftp, t->remote)) {
                conn_ok(g.sftp, sftp_remove_all(g.sftp, t->remote), "Cannot remove", t->remote);
                r = sftp_rename(g.sftp, t->tmp, t->remote);
        }
        if (r == SFTP_OK) return 'o';
        if (r != SFTP_ERR_IO) {
                LOG("Error", "Cannot put '%s:%s' in place: %s", g.host, t->remote, g.sftp->error);
                sftp_remove(g.sftp, t->tmp);
        }
        return 'e';
}

/* What was written to temp files on the remote, put in their places: the
 * agent does it with one lstat and one rename, so a change made there in
 * between isn't lost. What it couldn't do goes over SFTP. */
static void
place_pending(void)
{
        int n = placing.count;
        if (n == 0) return;
        SyncPlace *asks = calloc(n, sizeof *asks);
        assert(asks);
        for (int i = 0; i < n; i++) {
                Transfer *t = placing.items[i];
                asks[i]     = (SyncPlace) { .root   = t->root,
                                            .rel    = t->rel,
                                            .tmp    = rel_path(t->root->remote, t->tmp),
                                            .expect = &t->expect };
        }
        if (g.on_place(asks, n) == -1)
                for (int i = 0; i < n; i++)
                        asks[i].how = 'e';

        for (int i = 0; i < n; i++) {
                Transfer *t = placing.items[i];
                char how    = asks[i].how == 'e' ? place_over_sftp(t) : asks[i].how;
                if (how == 'c') VPRINT("File: %s [changed on the remote meanwhile, left alone]\n", t->remote);
                t->ok    = how == 'o';
                t->place = 0;
                transfer_done(t);
        }
        placing.count = 0;
        free(asks);
}

int
sync_drain(void)
{
        int died = pool_drain(transfer_done, progress_tick);
        place_pending();   // what was sent, into its place
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
sync_check(Root *root)
{
        return looks_wiped(root);
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
        return 0;
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
                                /* TO may have replaced another file: removed first */
                                expect(root, from, &(State) { 0 });
                                expect(root, to, &(State) { 0 });
                                expect(root, to, &M);
                                char *show_from = show_path(root, from), *show_to = show_path(root, to);
                                if (!g.quiet) say(stdout, "  ↑ %s → %s\n", show_from, show_to);
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

int
sync_remote_rename(Root *root, const char *from, const char *to, const State *seen)
{
        Record *rec = record_find(root, from);
        if (rec == NULL || record_find(root, to) || g.dry_run) return 0;
        /* A file or a directory (a symlink's target isn't in what the agent
         * says), still the one recorded */
        const State *R = &rec->st;
        if (seen->type != R->type || (R->type != 'f' && R->type != 'd') ||
            (R->type == 'f' && (seen->size != R->size || seen->mtime != R->mtime || seen->mode != R->mode)))
                return 0;

        const char *local_from = root_join(root->local, from);
        const char *local_to   = root_join(root->local, to);
        State L = local_state(local_from), T = local_state(local_to);
        int ok  = same_local(&L, R) && T.type == 0 && rename(local_from, local_to) == 0;
        if (ok) {
                char *show_from = show_path(root, from), *show_to = show_path(root, to);
                if (!g.quiet) say(stdout, "  ↓ %s → %s\n", show_from, show_to);
                g.received++;
                free(show_from);
                free(show_to);
                int is_file = R->type == 'f';
                record_rename(root, from, to); // R goes with it
                /* A rename moves a file's ctime: what's here now */
                if (is_file) {
                        State N = local_state(local_to);
                        record_set(root, to, &N);
                        state_free(&N);
                }
        }
        state_free(&L);
        state_free(&T);
        free((void *) local_from);
        free((void *) local_to);
        return ok;
}
