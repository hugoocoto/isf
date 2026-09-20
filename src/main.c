#define _DEFAULT_SOURCE

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "agent.h"
#include "bt.h"
#include "cum.h"
#include "flag.h"
#include "sftp.h"
#include "sync.h"
#include "update.h"
#include "util.h"
#include "watch.h"

#define HELP                                                                            \
        "Keeps a local folder and a remote one the same, both ways, over ssh.\n"        \
        "\n"                                                                            \
        "  isf ./proj server:proj      sync ./proj with ~/proj on server\n"             \
        "  isf ./proj                  again later: where it syncs to is remembered\n"  \
        "  isf                         the same, for the current folder\n"              \
        "  isf a b server:backups/     several folders, into backups/a and backups/b\n" \
        "  isf ./proj me@server:/srv/proj -p 2222\n"                                    \
        "\n"                                                                            \
        "When both sides change a file, the newest wins and the other one is kept as\n" \
        "FILE.isf-conflict. Files matching a line in the folder's .isfignore (like\n"   \
        ".gitignore) aren't synced, and neither are editor temp files. isf has to be\n" \
        "installed on the remote too."

/* Changes are sent when no event arrived for DEBOUNCE_MS, or MAX_DELAY_MS
 * after the first one if events keep coming */
#define DEBOUNCE_MS 100
#define MAX_DELAY_MS 1000

static struct {
        Da(Root) roots;
        Sftp sftp;
        int fd;           // inotify instance
        BT *dirty;        // per root, changes waiting to be sent: rel ("" for the
                          // root) -> its SYNC_* as a pointer
        int dirty_count;  // paths in them
        long dirty_since; // when the oldest of them arrived (ms)
        struct {          // a MOVED_FROM waiting for its MOVED_TO
                int active;
                uint32_t cookie;
                int root;
                char *rel;
                int is_dir;
        } move;
        const char *host;
        const char *port;         // NULL: ssh's default
        const char *isf;          // isf on the remote, NULL: in its PATH
        const char *port_flag;    // -p
        const char *isf_flag;     // -I
        const char *verbose_flag; // -v
        const char *quiet;        // -q
        const char *reset;        // --reset
        const char *jobs_flag;    // -j
        const char *dry_run;      // -n
        const char *once;         // --once
        const char *version;      // -V
        const char *update;       // --update
        const char *check;        // --check-update
        int jobs;                 // parallel transfer connections
        int save_dest;            // remember where the folders sync to
        char *ssh_opts[6];        // for every ssh session, NULL terminated
        Agent remote;             // reports changes on the remote
        int lost;                 // the connection broke: connect again
        BT made;                  // local directories isf made, until their IN_CREATE
        uint64_t *ign_stamp;      // per root: the ctime of the .isfignore loaded
} g;

#define LOG_SFTP(fmt, ...) LOG("Error", fmt ": %s", ##__VA_ARGS__, g.sftp.error)

/* Join -F and a local folder into the remote folder. "." and empty components
 * are dropped; ".." in the local folder is rejected (returns NULL) so the
 * result can't escape -F. */
static char *
remote_root(const char *folder, const char *local)
{
        char *buf = malloc(strlen(folder) + strlen(local) + 3);
        assert(buf);
        size_t len = 0;
        if (folder[0] == '/') buf[len++] = '/';

        const char *parts[] = { folder, local };
        for (int i = 0; i < 2; i++) {
                for (const char *p = parts[i]; *p;) {
                        size_t n = strcspn(p, "/");
                        if (i == 1 && n == 2 && !strncmp(p, "..", 2)) {
                                free(buf);
                                return NULL;
                        }
                        if (n > 0 && !(n == 1 && p[0] == '.')) {
                                if (len > 0 && buf[len - 1] != '/') buf[len++] = '/';
                                memcpy(buf + len, p, n);
                                len += n;
                        }
                        p += n;
                        if (*p) ++p; // the slash
                }
        }
        if (len == 0) buf[len++] = '.'; // remote home
        buf[len] = 0;
        return buf;
}

static int
valid_port(const char *port)
{
        size_t n = strspn(port, "0123456789");
        return n > 0 && n <= 5 && port[n] == 0 && atoi(port) >= 1 && atoi(port) <= 65535;
}

static long
now_ms(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* [user@]host:path, scp style: split it, or return 0 if ARG isn't one. A
 * ':' after a '/' is part of a local path. */
static int
split_remote(const char *arg, char **host, const char **path)
{
        const char *colon = strchr(arg, ':');
        const char *slash = strchr(arg, '/');
        if (colon == NULL || colon == arg || (slash && slash < colon)) return 0;
        *host = strndup(arg, colon - arg);
        *path = colon + 1;
        /* SFTP paths start at the home already */
        if (!strcmp(*path, "~")) *path += 1;
        if (!strncmp(*path, "~/", 2)) *path += 2;
        return 1;
}

/* Last component of LOCAL's real path, so "." has a name too */
static char *
folder_name(const char *local)
{
        char *abs = realpath(local, NULL);
        if (abs == NULL) return NULL;
        char *slash = strrchr(abs, '/');
        char *name  = strdup(slash && slash[1] ? slash + 1 : abs);
        free(abs);
        return name;
}

/* Where a folder syncs to, remembered in the state directory */
typedef struct {
        char *host;
        char *port; // NULL: ssh's default
        char *remote;
        char *isf; // NULL: in the remote PATH
} Dest;

static char *
dest_path(const char *local)
{
        char *abs = realpath(local, NULL);
        if (abs == NULL) return NULL;
        char name[32];
        snprintf(name, sizeof name, "dest-%016llx", (unsigned long long) hash_str(HASH_INIT, abs));
        free(abs);
        return state_path(name);
}

/* The file has host, port, remote and isf, each NUL terminated (empty for
 * NULL). Returns 1 if there is none. */
static int
dest_load(const char *local, Dest *d)
{
        char *path = dest_path(local);
        FILE *f    = path ? fopen(path, "r") : NULL;
        free(path);
        if (f == NULL) return 1;

        char *field[4] = { 0 };
        CharBuf buf    = { 0 };
        int c, n = 0;
        while (n < 4 && (c = fgetc(f)) != EOF) {
                Da_append(&buf, c);
                if (c) continue;
                field[n++] = buf.count > 1 ? strdup(buf.items) : NULL;
                buf.count  = 0;
        }
        fclose(f);
        Da_destroy(&buf);
        if (n < 4 || !field[0] || !field[2]) return 1;
        *d = (Dest) { field[0], field[1], field[2], field[3] };
        return 0;
}

static void
dest_free(Dest *d)
{
        free(d->host);
        free(d->port);
        free(d->remote);
        free(d->isf);
        *d = (Dest) { 0 };
}

static void
dest_save(const char *local, const Dest *d)
{
        char *path = dest_path(local);
        FILE *f    = path ? fopen(path, "w") : NULL;
        if (f) {
                const char *field[] = { d->host, d->port, d->remote, d->isf };
                for (int i = 0; i < 4; i++) {
                        fputs(field[i] ? field[i] : "", f);
                        fputc(0, f);
                }
                fclose(f);
        }
        if (!f) LOG_ERR("Cannot remember where '%s' syncs to", local);
        free(path);
}

static int
same_str(const char *a, const char *b)
{
        return a == b || (a && b && !strcmp(a, b));
}

/* The arguments are local folders, then the remote as [user@]host:folder.
 * One folder syncs with that remote folder; several (or a remote ending in
 * '/') go inside it, with their names. Without the remote, each folder syncs
 * where it did last time; without folders, the current one. */
static int
parse_args(int argc, char **argv)
{
        char *host       = NULL;
        const char *path = NULL;
        int count        = argc - 1;
        if (count > 0 && split_remote(argv[argc - 1], &host, &path)) count--;

        char *here[]  = { "." };
        char **locals = count ? argv + 1 : here;
        if (count == 0) count = 1;
        if (count > 255) {
                fprintf(stderr, "isf: too many folders, 255 at most\n");
                return 1;
        }
        int inside_it = count > 1 || (path && (*path == 0 || path[strlen(path) - 1] == '/'));

        for (int i = 0; i < count; i++) {
                const char *local = locals[i];
                struct stat st;
                if (stat(local, &st) == -1 || !S_ISDIR(st.st_mode)) {
                        fprintf(stderr, "isf: '%s' isn't a folder\n", local);
                        return 1;
                }

                Dest d = { 0 }, before = { 0 };
                if (host) {
                        /* Same host as last time: its port and isf still apply */
                        if (!dest_load(local, &before)) {
                                if (!strcmp(before.host, host)) {
                                        d.port      = before.port;
                                        d.isf       = before.isf;
                                        before.port = before.isf = NULL;
                                }
                                dest_free(&before);
                        }
                        char *name = folder_name(local);
                        d.host     = host;
                        d.remote   = inside_it ? remote_root(*path ? path : ".", name) : remote_root(path, "");
                        free(name);
                } else if (dest_load(local, &d)) {
                        fprintf(stderr, "isf: where does '%s' sync to? The first time, say it: isf %s host:folder\n",
                                local, local);
                        return 1;
                }
                if (g.port_flag) {
                        free(d.port);
                        d.port = (char *) g.port_flag;
                }
                if (g.isf_flag) {
                        free(d.isf);
                        d.isf = (char *) g.isf_flag;
                }

                /* There is a single connection */
                if (i > 0 && (strcmp(d.host, g.host) || !same_str(d.port, g.port))) {
                        fprintf(stderr, "isf: '%s' and '%s' sync to different hosts: run isf once for each\n",
                                locals[0], local);
                        return 1;
                }
                g.host = d.host;
                g.port = d.port;
                if (d.isf) g.isf = d.isf;
                Da_append(&g.roots, (Root) { .local = local, .remote = d.remote });
        }

        if (g.port && !valid_port(g.port)) {
                fprintf(stderr, "isf: '%s' isn't a port\n", g.port);
                return 1;
        }
        g.save_dest = host || g.port_flag || g.isf_flag;
        return 0;
}

/* Remember that REL, in root ROOT, has to be sent */
static void
mark(int root, const char *rel, int what)
{
        intptr_t had = (intptr_t) bt_get(&g.dirty[root], rel);
        if (had == 0 && g.dirty_count++ == 0) g.dirty_since = now_ms();
        bt_add(&g.dirty[root], rel, (void *) (had | what));
}

/* Look at everything in ROOT again: its .isfignore first (the flush syncs it
 * before the rest, and loads its patterns), then all of it */
static void
rescan(int root)
{
        mark(root, IGNORE_FILE, SYNC_DATA);
        mark(root, "", SYNC_TREE);
}

/* The ctime of ROOT's .isfignore here (0 if there is none) */
static uint64_t
ignore_stamp(const Root *root)
{
        const char *path = pathjoin(root->local, IGNORE_FILE);
        struct stat st;
        uint64_t stamp = lstat(path, &st) == 0 ? st.st_ctim.tv_sec * 1000000000ull + st.st_ctim.tv_nsec : 0;
        free((void *) path);
        return stamp;
}

/* Is REL inside one of the directories in COVERED? */
static int
covered_by(BT *covered, const char *rel)
{
        if (bt_get(covered, "")) return 1; // the root: everything
        char *path = strdup(rel);
        int hit    = 0;
        for (char *slash = path; !hit && (slash = strchr(slash, '/')); *slash++ = '/') {
                *slash = 0;
                hit    = bt_get(covered, path) != NULL;
        }
        free(path);
        return hit;
}

/* Wait for the parallel transfers started so far. If one's connection broke,
 * so did the others (they share it): connect again after this flush. */
static void
drain(void)
{
        if (sync_drain()) g.lost = 1;
}

/* Send every change waiting in g.dirty */
static void
flush(void)
{
        if (g.dirty_count == 0) return;
        sync_progress(1);

        /* Again while isf itself leaves something for the other side (a file
         * kept aside as a conflict copy is new to it), so one run of syncing
         * brings both sides together */
        for (int round = 0; round < 4; round++) {
                for (int i = 0; i < g.roots.count; i++) {
                        Root *root = &g.roots.items[i];
                        BT *dirty  = &g.dirty[i];

                        /* A changed .isfignore goes first, then the folder is looked at
                         * again whole with the new patterns: the paths marked with it
                         * were filtered with the old ones, and what they don't ignore
                         * anymore has no events. */
                        intptr_t what = (intptr_t) bt_get(dirty, IGNORE_FILE);
                        if (what) {
                                int sent = sync_stats().sent;
                                reconcile(root, IGNORE_FILE, what, NULL);
                                drain(); // a download of it may be in the pool
                                /* Sent: the remote folder isn't what was listed */
                                if (sync_stats().sent != sent) sync_forget_seed(root);
                                ignore_load(&root->ign, root->local);
                                g.ign_stamp[i] = ignore_stamp(root);
                                mark(i, "", SYNC_TREE);
                        }

                        /* In order, a directory comes before what's inside it. If it
                         * was handled whole, the paths inside are already done. */
                        BT covered = { 0 };
                        BT *d;
                        for_bt_each(d, dirty)
                        {
                                if (covered_by(&covered, d->key)) continue;
                                struct stat st;
                                if (*d->key == 0 && lstat(root->local, &st) == -1)
                                        LOG_WARN("'%s' is gone, the remote folder is left as it is", root->local);
                                else if (reconcile(root, d->key, (intptr_t) d->value, NULL))
                                        bt_add(&covered, d->key, (void *) 1);
                        }
                        bt_destroy(&covered);
                        bt_destroy(dirty);
                }
                g.dirty_count = 0;

                /* Wait for the parallel transfers this batch started, then save */
                drain();
                SyncExtra *extra;
                int kept = sync_take_extra(&extra);
                for (int i = 0; i < kept; i++) {
                        Root *root = extra[i].root;
                        if (!ignored(&root->ign, extra[i].rel, 0)) mark((int) (root - g.roots.items), extra[i].rel, SYNC_DATA);
                        free(extra[i].rel);
                }
                free(extra);
                if (g.dirty_count == 0) break;
        }
        sync_progress(0);
        Da_foreach(root, g.roots)
        {
                record_save(root);
                sync_forget_seen(root);
        }

        /* A .isfignore this flush wrote here (received from the remote)
         * hasn't been loaded: load it, and look at everything again with it */
        for (int i = 0; i < g.roots.count; i++) {
                Root *root = &g.roots.items[i];
                if (ignore_stamp(root) == g.ign_stamp[i]) continue;
                ignore_load(&root->ign, root->local);
                g.ign_stamp[i] = ignore_stamp(root);
                mark(i, "", SYNC_TREE);
        }
}

/* The pending MOVED_FROM got no MOVED_TO: it left the watched folders */
static void
moved_out(void)
{
        if (!g.move.active) return;
        mark(g.move.root, g.move.rel, SYNC_DATA);
        if (g.move.is_dir) {
                /* Its watches would keep reporting it under the old path */
                const char *local = root_join(g.roots.items[g.move.root].local, g.move.rel);
                unwatch(local, g.fd);
                free((void *) local);
        }
        free(g.move.rel);
        g.move.active = 0;
}

/* FROM was renamed to TO in root ROOT_I, on one side, and the other side too:
 * the changes waiting under the old name are now at the new one */
static void
dirty_renamed(int root_i, const char *from, const char *to)
{
        BT *dirty       = &g.dirty[root_i];
        Da(char *) keys = { 0 };
        BT *d;
        for_bt_each(d, dirty)
        {
                if (!strcmp(d->key, from) || inside(d->key, from)) Da_append(&keys, strdup(d->key));
        }
        size_t from_len = strlen(from);
        Da_foreach(k, keys)
        {
                intptr_t what = (intptr_t) bt_get(dirty, *k);
                bt_del(dirty, *k);
                g.dirty_count--;
                const char *rest = *k + from_len;
                char *rel        = malloc(strlen(to) + strlen(rest) + 1);
                assert(rel);
                strcpy(stpcpy(rel, to), rest);
                mark(root_i, rel, what);
                free(rel);
                free(*k);
        }
        Da_destroy(&keys);
}

/* FROM was renamed to TO inside a root: rename the remote copy instead of
 * sending it again */
static void
moved(int root_i, const char *from, const char *to, int is_dir)
{
        Root *root = &g.roots.items[root_i];
        /* isf's own doing: a rename it received from the remote */
        if (sync_unchanged(root, from) && sync_unchanged(root, to)) return;
        if (sync_rename(root, from, to)) {
                dirty_renamed(root_i, from, to);
        } else {
                mark(root_i, from, SYNC_DATA);
                mark(root_i, to, is_dir ? SYNC_TREE : SYNC_DATA);
        }
}

/* Every ssh session (sftp, the agent, the transfers) shares one connection,
 * so there is a single login: the first one becomes the master. The socket
 * goes in a private directory. Keepalives notice a connection that went
 * silent (a network that went away) in under a minute, to connect again. */
static void
make_ssh_opts(void)
{
        static char path[PATH_MAX];
        const char *dir = getenv("XDG_RUNTIME_DIR");
        snprintf(path, sizeof path, "-oControlPath=%s/isf-%%C", dir && *dir ? dir : "~/.ssh");
        g.ssh_opts[0] = "-oControlMaster=auto";
        g.ssh_opts[1] = path;
        g.ssh_opts[2] = "-oControlPersist=no";
        g.ssh_opts[3] = "-oServerAliveInterval=15";
        g.ssh_opts[4] = "-oServerAliveCountMax=3";
        g.ssh_opts[5] = NULL;
}

/* "N thing" or "N things" */
static void
print_count(const char *sep, int n, const char *one, const char *many)
{
        printf("%s%d %s", sep, n, n == 1 ? one : many);
}

/* After the first sync. ERRORS: how many were said during it. */
static void
print_summary(int errors)
{
        SyncStats st = sync_stats();
        if (st.sent + st.received + st.conflicts + errors == 0) {
                printf("isf: already in sync\n");
                return;
        }
        if (g.dry_run) {
                printf("isf: dry run, nothing was changed:");
                print_count(" ", st.sent, "to send", "to send");
                print_count(", ", st.received, "to receive", "to receive");
        } else {
                printf(errors ? "isf: not all in sync:" : "isf: in sync:");
                print_count(" ", st.sent, "sent", "sent");
                print_count(", ", st.received, "received", "received");
        }
        if (st.conflicts) print_count(", ", st.conflicts, "conflict", "conflicts");
        if (errors) print_count(", ", errors, "error (see above)", "errors (see above)");
        printf("\n");
}

/* How to copy this isf to the remote */
static void
copy_hint(void)
{
        /* In an AppImage, /proc/self/exe is inside its mount, gone when isf
         * exits: the AppImage itself is the file to copy */
        char self[PATH_MAX];
        ssize_t n            = readlink("/proc/self/exe", self, sizeof self - 1);
        self[n > 0 ? n : 0]  = 0;
        const char *appimage = getenv("APPIMAGE");
        if (appimage && *appimage) snprintf(self, sizeof self, "%s", appimage);
        fprintf(stderr,
                "     Copy it, for example:\n"
                "         scp %s%s%s%s %s:.local/bin/isf\n"
                "     and if ~/.local/bin isn't in the PATH of ssh commands there, say where it is\n"
                "     (it's remembered):\n"
                "         isf %s %s:%s -I .local/bin/isf\n",
                g.port ? "-P " : "", g.port ? g.port : "", g.port ? " " : "", *self ? self : "isf", g.host,
                g.roots.items[0].local, g.host, g.roots.items[0].remote);
}

/* The agent exited before it was ready. STATUS 126/127 is the shell on the
 * remote not finding isf or not being able to run it. */
static void
agent_didnt_start(int status)
{
        if (status != 126 && status != 127) {
                fprintf(stderr, "isf: isf on '%s' stopped before it started (exit status %d)\n", g.host, status);
                return;
        }
        fprintf(stderr, "isf: isf has to be installed on '%s' too, and it isn't (or it isn't in the PATH of ssh\n"
                        "     commands there).\n",
                g.host);
        copy_hint();
}

/* The agent speaks another protocol: another version of isf */
static void
agent_mismatch(void)
{
        fprintf(stderr, "isf: isf on '%s' is %s, and this one is %s: they don't work together.\n"
                        "     The same isf has to be on both sides. With releases, 'isf --update'\n"
                        "     on each takes the newest one.\n",
                g.host, g.remote.version ? g.remote.version : "an older version", VERSION);
        copy_hint();
}

/* Start the agent and wait until it watches. AGAIN: connecting again, when
 * the agent not starting may be the connection still failing. Returns 0, 1 if
 * it can be tried again, or 2 (logged) if not. */
static void remote_change(char type, int root, const char *rel, const State *seen);
static void remote_moved(int root, const char *from, const char *to, const State *seen);

static void remote_listed(int root, const char *rel, const SftpAttrs *self, SftpDir *dir);

/* The answers to what isf asked the agent to put in place, by request id */
static struct {
        uint64_t first; // the id of places[0]
        SyncPlace *of;  // [count], what was asked
        int count;
        int left; // still without an answer
} asked;

/* One of them is done: 'o', 'c' (it changed there) or 'e' */
static void
remote_placed(uint64_t id, char how)
{
        if (id < asked.first || id >= asked.first + (uint64_t) asked.count) return;
        SyncPlace *p = &asked.of[id - asked.first];
        if (p->how) return; // answered already
        p->how = how;
        asked.left--;
}

/* Have the agent put each of PLACES in place: it checks and renames there,
 * so nothing can change in between. Returns 0, or -1 if the agent is gone
 * (then they keep their temp files, and isf tries again over SFTP). */
static int
place_files(SyncPlace *places, int n)
{
        /* In chunks: the agent answers, and says what it sees, while isf asks.
         * Asking for everything at once could fill both pipes and leave the
         * two of them waiting for each other. */
        enum { CHUNK = 128 };
        static uint64_t next_id;
        if (g.remote.pid <= 0 || n <= 0) return -1;

        for (int done = 0; done < n;) {
                int m = n - done < CHUNK ? n - done : CHUNK;
                asked = (typeof(asked)) { .first = next_id, .of = places + done, .count = m, .left = m };
                for (int i = 0; i < m; i++) {
                        SyncPlace *p     = &places[done + i];
                        const State *e   = p->expect;
                        unsigned type    = e->type == 'f' ? S_IFREG : e->type == 'd' ? S_IFDIR : e->type == 'l' ? S_IFLNK : 0;
                        SftpAttrs expect = { .size = e->size, .mtime = e->mtime, .perm = e->mode | type };
                        p->how           = 0;
                        if (agent_place(&g.remote, (int) (p->root - g.roots.items), next_id + i,
                                        e->type ? &expect : NULL, p->tmp, p->rel)) {
                                asked = (typeof(asked)) { 0 };
                                return -1;
                        }
                }
                next_id += m;
                /* Their answers come with everything else the agent says */
                while (asked.left > 0) {
                        if (agent_read(&g.remote, remote_change, remote_moved, remote_listed, remote_placed)) {
                                asked  = (typeof(asked)) { 0 };
                                g.lost = 1;
                                return -1;
                        }
                }
                done += m;
        }
        asked = (typeof(asked)) { 0 };
        return 0;
}

/* The agent's listing of a folder, as it started */
static void
remote_listed(int root, const char *rel, const SftpAttrs *self, SftpDir *dir)
{
        if (root >= g.roots.count) {
                if (dir) sftp_dir_free(dir);
                return;
        }
        if (dir && !path_safe(rel)) { // damaged, like one that can't be read
                sftp_dir_free(dir);
                dir = NULL;
        }
        sync_remote_listed(&g.roots.items[root], rel, self, dir);
}
static int
start_agent(int again)
{
        Da_foreach(r, g.roots)
        {
                sync_forget_seed(r); // listed by an agent before
        }
        if (agent_start(&g.remote, g.isf ? g.isf : "isf", g.host, g.port, g.ssh_opts, g.roots.items, g.roots.count))
                return 2;
        while (!g.remote.ready) {
                if (agent_read(&g.remote, remote_change, remote_moved, remote_listed, remote_placed)) {
                        int status = agent_stop(&g.remote);
                        if (again && status != 126 && status != 127) return 1;
                        agent_didnt_start(status);
                        return 2;
                }
        }
        /* Both clocks decide which side changed a file last */
        if (g.remote.time) {
                long long skew = (long long) g.remote.time - (long long) time(NULL);
                if (skew > 5 || skew < -5)
                        LOG_WARN("The clock on '%s' is %lld seconds %s this one: which side changed a file last "
                                 "can be wrong. Keep both in time (NTP).",
                                 g.host, skew < 0 ? -skew : skew, skew > 0 ? "ahead of" : "behind");
        }
        /* The same isf on both sides: a version that isn't this one may say
         * the same protocol and still mean something else by it */
        if (g.remote.protocol != AGENT_PROTOCOL || g.remote.version == NULL || strcmp(g.remote.version, VERSION)) {
                agent_mismatch();
                agent_stop(&g.remote);
                return 2;
        }
        return 0;
}

/* The connection broke: close what's left of it and connect again, waiting
 * longer each time. Then everything is looked at again, like at the start:
 * the changes made meanwhile, on either side, are in the comparison. */
static void
reconnect(void)
{
        agent_stop(&g.remote);
        sync_shutdown();
        sftp_disconnect(&g.sftp);
        LOG_WARN("Lost the connection to '%s', connecting again", g.host);
        for (int wait = 1;; wait = wait < 30 ? 2 * wait : 60) {
                sleep(wait);
                if (sftp_connect(&g.sftp, g.host, g.port, g.ssh_opts)) continue;
                sync_init(&g.sftp, g.host, g.port, g.ssh_opts, g.jobs, 0, g.quiet != NULL);
                int st = start_agent(1);
                if (st != 0) {
                        sync_shutdown();
                        sftp_disconnect(&g.sftp);
                        if (st == 2) exit(1);
                        continue;
                }
                /* A side that's empty now (a disk lost meanwhile) stops it */
                Da_foreach(r, g.roots)
                {
                        if (sync_check(r)) exit(1);
                }
                break;
        }
        g.lost = 0;
        printf("isf: connected to '%s' again\n", g.host);
        for (int i = 0; i < g.roots.count; i++)
                rescan(i);
}

/* The agent reported FROM renamed to TO on the remote, and saw TO as SEEN */
static void
remote_moved(int root, const char *from, const char *to, const State *seen)
{
        if (root >= g.roots.count) return;
        if (!path_safe(from) || !path_safe(to) || !*from || !*to) {
                LOG_WARN("Ignoring an unsafe rename from the remote: '%s' to '%s'", from, to);
                return;
        }
        Root *r    = &g.roots.items[root];
        int is_dir = seen->type == 'd';
        if (!ignored(&r->ign, from, is_dir) && !ignored(&r->ign, to, is_dir) && sync_remote_rename(r, from, to, seen)) {
                dirty_renamed(root, from, to);
                return;
        }
        /* It can't be done the same way here: as a removal and something new */
        const State gone = { 0 };
        remote_change(is_dir ? 'D' : 'C', root, from, &gone);
        remote_change(is_dir ? 'D' : 'C', root, to, seen);
}

/* A change the agent reported on the remote */
static void
remote_change(char type, int root, const char *rel, const State *seen)
{
        if (root >= g.roots.count) return;
        if (type == 'O') {
                rescan(root);
                return;
        }
        /* The remote could name a path that leaves the folder (../..): never
         * let it build a local path */
        if (!path_safe(rel)) {
                LOG_WARN("Ignoring an unsafe path from the remote: '%s'", rel);
                return;
        }
        /* 'C', or 'D' if it's a directory */
        if (ignored(&g.roots.items[root].ign, rel, type == 'D')) return;
        mark(root, rel, SYNC_DATA);
        if (seen) sync_remote_seen(&g.roots.items[root], rel, seen, type);
}

/* A local change at REL (PATH), unless it's still what was last synced: then
 * the event is isf's own doing (a file it received), and looking at it again
 * would only cost a round trip. A directory that appears is looked at whole,
 * unless isf made it: what's in another one doesn't show in its own state. */
static void
mark_local(int root, const char *path, const char *rel, int what)
{
        if (what & SYNC_TREE) {
                if (bt_get(&g.made, path)) {
                        bt_del(&g.made, path);
                        return;
                }
        } else if (sync_unchanged(&g.roots.items[root], rel)) {
                return;
        }
        mark(root, rel, what);
}

/* A file written to without being closed (a log), now that the writes
 * stopped for a while */
static void
held_local(const char *path, int root)
{
        mark_local(root, path, rel_path(g.roots.items[root].local, path), SYNC_DATA);
}

/* sync made a local directory: watch it now, before anything goes in it,
 * so its own event needn't send for all of it */
static void
watch_new_dir(Root *root, const char *path)
{
        listen_folder(path, root - g.roots.items, g.fd);
        bt_add(&g.made, path, (void *) 1);
}

static void
handle_event(const struct inotify_event *event, int fd)
{
        if (event->mask & IN_Q_OVERFLOW) {
                LOG_WARN("Event queue overflowed, some events were lost: checking everything");
                moved_out();
                for (int i = 0; i < g.roots.count; i++)
                        rescan(i);
                return;
        }

        if (event->len && is_temp_name(event->name)) return; // our own transfer

        /* A MOVED_FROM is followed by its MOVED_TO. If this isn't it, the file
         * or directory was moved out of the watched folders. */
        if (g.move.active && !((event->mask & IN_MOVED_TO) && event->cookie == g.move.cookie))
                moved_out();

        Watch *w = find_watch(event->wd);
        if (w == NULL) return; // watch already removed

        /* Keep the index: listen_folder can realloc the watch list under w */
        int root_i       = w->root;
        const Root *root = &g.roots.items[root_i];

        if (event->mask & IN_IGNORED) {
                /* The watch is gone (directory deleted, unmounted...) */
                forget_watch(w);
                return;
        }

        /* len == 0 means the event is about the watched directory itself */
        const char *path = event->len ? pathjoin(w->path, event->name) : strdup(w->path);
        const char *rel  = rel_path(root->local, path);
        int is_dir       = (event->mask & IN_ISDIR) || event->len == 0;
        int is_root      = !strcmp(path, root->local);
        int renamed      = 0; // MOVED_TO paired with the pending MOVED_FROM

        if (event->len && ignored(&root->ign, rel, is_dir)) {
                VPRINT("File: %s [ignored]\n", path);
                free((void *) path);
                return;
        }

        VPRINT("File: %s", path); /* Print the name of the file.  */
        if (is_dir) {             /* Print type of filesystem object.  */
                VPRINT(" [directory]");
        } else {
                VPRINT(" [file]");
        }

        VPRINT(" Event: "); /* Print event type.  */

        if (event->mask & IN_ACCESS) { // File was accessed (e.g., read(2), execve(2)).
                VPRINT("IN_ACCESS ");
        }

        if (event->mask & IN_ATTRIB) { // Metadata  changed——for  example, permissions (e.g., chmod(2)), timestamps (e.g., utimensat(2)), extended attributes (setxattr(2)), link count (since Linux 2.6.25; e.g., for the target of link(2) and for unlink(2)), and user/group ID (e.g., chown(2)).
                VPRINT("IN_ATTRIB ");
                if (event->len) mark_local(root_i, path, rel, SYNC_ATTR);
        }

        if (event->mask & IN_CLOSE_WRITE) { // File opened for writing was closed.
                VPRINT("IN_CLOSE_WRITE ");
                held_forget(path);
                mark_local(root_i, path, rel, SYNC_DATA);
        }

        if (event->mask & IN_CLOSE_NOWRITE) { // File or directory not opened for writing was closed.
                VPRINT("IN_CLOSE_NOWRITE ");
        }

        if (event->mask & IN_CREATE) { // File/directory created in watched directory (e.g., open(2) O_CREAT, mkdir(2),  link(2),  symlink(2),  bind(2)  on  a  UNIX  domain socket).
                VPRINT("IN_CREATE ");
                /* Watch a new directory first, then send all of it: things
                 * created in it before the watch have no events */
                if (is_dir) listen_folder(path, root_i, fd);
                mark_local(root_i, path, rel, is_dir ? SYNC_TREE : SYNC_DATA);
        }

        if (event->mask & IN_DELETE) { // File/directory deleted from watched directory.
                VPRINT("IN_DELETE ");
                held_forget(path);
                mark_local(root_i, path, rel, SYNC_DATA);
        }

        if (event->mask & IN_DELETE_SELF) { // Watched  file/directory  was  itself deleted.  (This event also occurs if an object is moved to another filesystem, since mv(1) in effect copies the file to the other filesystem and then deletes it from the original  filesystem.)   In  addition,  an  IN_IGNORED event will subsequently be generated for the watch descriptor.
                VPRINT("IN_DELETE_SELF ");
                if (is_root) LOG_WARN("'%s' was removed, it's not synced anymore", path);
        }

        if (event->mask & IN_MODIFY) { // File was modified (e.g., write(2), truncate(2)).
                VPRINT("IN_MODIFY ");  // sent at its IN_CLOSE_WRITE, or once the writes stop
                if (!is_dir) held_write(path, root_i);
        }

        if (event->mask & IN_MOVE_SELF) { // Watched file/directory was itself moved.
                VPRINT("IN_MOVE_SELF ");
                if (is_root) {
                        /* Its watches would keep reporting it under the old path */
                        LOG_WARN("'%s' was moved, it's not synced anymore", path);
                        unwatch(path, g.fd);
                }
        }

        if (event->mask & IN_MOVED_FROM) { // Generated for the directory containing the old filename when a file is renamed.
                VPRINT("IN_MOVED_FROM ");
                held_forget(path);
                g.move.active = 1;
                g.move.cookie = event->cookie;
                g.move.root   = root_i;
                g.move.rel    = strdup(rel);
                g.move.is_dir = is_dir;
        }

        if (event->mask & IN_MOVED_TO) { // Generated for the directory containing the new filename when a file is renamed.
                VPRINT("IN_MOVED_TO ");
                if (g.move.active && g.move.cookie == event->cookie && g.move.root == root_i) {
                        renamed = 1; // done below, after printing the event
                } else {
                        /* Moved in from outside, or from another root (or a
                         * file isf received, renamed from its temp file) */
                        moved_out();
                        if (is_dir)
                                mark(root_i, rel, SYNC_TREE); // isf makes its directories, it doesn't move them in
                        else
                                mark_local(root_i, path, rel, SYNC_DATA);
                }
                /* A renamed directory keeps its watches (same wds), this
                 * updates their paths */
                if (is_dir) listen_folder(path, root_i, fd);
        }

        if (event->mask & IN_OPEN) { // File or directory was opened.
                VPRINT("IN_OPEN ");
        }
        VPRINT("\n");

        if (renamed) {
                moved(root_i, g.move.rel, rel, is_dir);
                free(g.move.rel);
                g.move.active = 0;
        }
        free((void *) path);
}

/* Wait for events on both sides and send them, until the agent stops on its
 * own or something fails. Returns 1 then. */
static int
watch_loop(void)
{
        for (;;) {
                if (g.lost || sync_lost()) reconnect();

                struct pollfd fds[] = {
                        { .fd = g.fd, .events = POLLIN },           // inotify
                        { .fd = g.remote.from, .events = POLLIN }, // the agent
                };
                /* With changes waiting, wait only DEBOUNCE_MS for more, or
                 * until a file held open is due */
                int pending  = g.dirty_count > 0 || g.move.active;
                int wait     = pending ? DEBOUNCE_MS : -1;
                int held     = held_timeout();
                int for_held = held >= 0 && (wait < 0 || held < wait);
                int poll_num = poll(fds, 2, for_held ? held : wait);
                if (poll_num == -1) {
                        if (errno == EINTR) continue;
                        LOG_ERR("poll");
                        return 1;
                }
                held_due(held_local);

                if (fds[0].revents & POLLIN) {
                        if (handle_events(g.fd, handle_event)) return 1;
                }
                if (fds[1].revents && agent_read(&g.remote, remote_change, remote_moved, remote_listed, remote_placed)) {
                        /* Exit status 1 is the agent stopping on its own: its
                         * folder was removed. Anything else (ssh's 255, a
                         * signal) is the connection. */
                        if (agent_stop(&g.remote) == 1) {
                                LOG_WARN("The agent on the remote stopped, stopping");
                                return 1;
                        }
                        g.lost = 1;
                        continue;
                }

                if (poll_num == 0 && !for_held) {
                        /* Quiet: a MOVED_TO would have arrived by now */
                        moved_out();
                        flush();
                } else if (g.dirty_count > 0 && now_ms() - g.dirty_since >= MAX_DELAY_MS) {
                        flush();
                }
        }
}

int
main(int argc, char **argv)
{
        /* isf starts the agent itself on the remote: it isn't in the help */
        if (argc > 1 && !strcmp(argv[1], "--agent")) {
                for (int i = 2; i < argc && i < 2 + 255; i++)
                        Da_append(&g.roots, (Root) { .local = argv[i] });
                return agent_main(g.roots.items, g.roots.count);
        }

        flag_program(.name = "isf [folder...] [[user@]host:folder]", .help = HELP);
        flag_add(&g.port_flag, "--port", "-p", .nargs = 1,
                 .help = "ssh port, if not the default or the one in ~/.ssh/config");
        flag_add(&g.isf_flag, "--isf", "-I", .nargs = 1,
                 .help = "where isf is on the remote, if it isn't in the PATH of ssh commands there");
        flag_add(&g.verbose_flag, "--verbose", "-v", .help = "show every event, and where errors come from");
        flag_add(&g.quiet, "--quiet", "-q", .help = "don't list each file sent or received");
        flag_add(&g.reset, "--reset", .help = "forget what was synced before: sync like the first time");
        flag_add(&g.jobs_flag, "--jobs", "-j", .nargs = 1, .defaults = "4",
                 .help = "how many files to transfer at once (parallel connections)");
        flag_add(&g.dry_run, "--dry-run", "-n", .help = "show what syncing would do, change nothing, and exit");
        flag_add(&g.once, "--once", .help = "sync what's different now and exit, instead of watching for changes");
        flag_add(&g.version, "--version", "-V", .help = "show the version");
        flag_add(&g.check, "--check-update", .help = "say whether a newer isf has been released (exit status 1 if there is)");
        flag_add(&g.update, "--update", .help = "replace this isf with the newest release, if it isn't this one");

        /* flag_free() frees the values, so it waits until the end */
        if (flag_parse(&argc, &argv)) {
                flag_show_help(STDOUT_FILENO);
                exit(1);
        }
        if (g.version) {
                printf("isf %s\n", VERSION);
                flag_free();
                return 0;
        }
        verbose = g.verbose_flag != NULL;
        if (g.update || g.check) {
                int r = update_run(g.update != NULL);
                flag_free();
                return r;
        }
        if (parse_args(argc, argv)) return 1;
        g.dirty     = calloc(g.roots.count, sizeof *g.dirty);
        g.ign_stamp = calloc(g.roots.count, sizeof *g.ign_stamp);
        assert(g.dirty && g.ign_stamp);
        g.jobs = atoi(g.jobs_flag);
        if (g.jobs < 1 || g.jobs > 64) {
                fprintf(stderr, "isf: -j must be between 1 and 64\n");
                return 1;
        }

        /* Print events as they happen, even if stdout is a pipe */
        setvbuf(stdout, NULL, _IOLBF, 0);

        make_ssh_opts();
        if (sftp_connect(&g.sftp, g.host, g.port, g.ssh_opts)) {
                LOG_SFTP("Cannot connect to '%s'", g.host);
                return 1;
        }
        sync_init(&g.sftp, g.host, g.port, g.ssh_opts, g.jobs, g.dry_run != NULL, g.quiet != NULL);
        Da_foreach(r, g.roots)
        {
                if (g.save_dest && !g.dry_run)
                        dest_save(r->local, &(Dest) { (char *) g.host, (char *) g.port, r->remote, (char *) g.isf });
        }

        Da_foreach(r, g.roots)
        {
                printf("isf: %s ⇄ %s:%s\n", r->local, g.host, r->remote);
                if (g.roots.count > 1) r->label = r->local;
        }

        Da_foreach(r, g.roots)
        {
                if (sync_open(r, g.reset != NULL)) return 1;
                g.ign_stamp[r - g.roots.items] = ignore_stamp(r);
        }

        if (g.dry_run) {
                Da_foreach(r, g.roots)
                {
                        if (sync_check(r)) return 1;
                }
                int errors = logged_errors();
                sync_progress(1);
                Da_foreach(r, g.roots)
                {
                        reconcile(r, "", SYNC_TREE, NULL);
                }
                sync_progress(0);
                print_summary(logged_errors() - errors);
                sftp_disconnect(&g.sftp);
                flag_free();
                return 0;
        }

        /* Changes on the remote are watched from before the first sync (the
         * agent makes the remote folders if they're missing), and from
         * before it's listed */
        if (start_agent(0)) return 1;
        Da_foreach(r, g.roots)
        {
                if (sync_check(r)) {
                        agent_stop(&g.remote);
                        return 1;
                }
        }

        int fd = watch_init();
        if (fd < 0) return 1;
        g.fd = fd;
        sync_on_local_dir(watch_new_dir);
        sync_on_place(place_files); // the agent puts what isf sends in place

        for (int i = 0; i < g.roots.count; i++) {
                if (listen_folder(g.roots.items[i].local, i, fd)) return 1;
        }

        /* Bring both sides together. After the watches, so what changes
         * meanwhile isn't lost: its events come after this. */
        for (int i = 0; i < g.roots.count; i++)
                rescan(i);
        int errors = logged_errors();
        flush();
        print_summary(logged_errors() - errors);
        int ret = 0;
        if (g.once) {
                ret = logged_errors() - errors ? 1 : 0;
        } else {
                printf("isf: watching for changes, Ctrl-C to stop\n");
                ret = watch_loop();
        }
        agent_stop(&g.remote);
        sync_shutdown();
        sftp_disconnect(&g.sftp);
        flag_free();
        return ret;
}
