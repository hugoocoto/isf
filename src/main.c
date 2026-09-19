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
#include "util.h"
#include "watch.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

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

typedef Da(struct pollfd) watch_pollfds;

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
        const char *reset;        // --reset
        const char *jobs_flag;    // -j
        const char *dry_run;      // -n
        const char *version;      // -V
        int jobs;                 // parallel transfer connections
        int save_dest;            // remember where the folders sync to
        char *ssh_opts[4];        // for both ssh sessions, NULL terminated
        Agent remote;             // reports changes on the remote
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

/* Wait for the parallel transfers started so far. A broken transfer
 * connection can't be recovered. */
static void
drain(void)
{
        if (sync_drain()) {
                Da_foreach(root, g.roots)
                {
                        record_save(root);
                }
                LOG_WARN("A transfer connection broke, stopping");
                sync_shutdown();
                exit(1);
        }
}

/* Send every change waiting in g.dirty */
static void
flush(void)
{
        if (g.dirty_count == 0) return;

        for (int i = 0; i < g.roots.count; i++) {
                Root *root = &g.roots.items[i];
                BT *dirty  = &g.dirty[i];

                /* A changed .isfignore goes first, then the folder is looked at
                 * again whole with the new patterns: the paths marked with it
                 * were filtered with the old ones, and what they don't ignore
                 * anymore has no events. */
                intptr_t what = (intptr_t) bt_get(dirty, IGNORE_FILE);
                if (what) {
                        reconcile(root, IGNORE_FILE, what, NULL);
                        drain(); // a download of it may be in the pool
                        ignore_load(&root->ign, root->local);
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
        Da_foreach(root, g.roots)
        {
                record_save(root);
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

/* FROM was renamed to TO inside a root: rename the remote copy instead of
 * sending it again */
static void
moved(int root_i, const char *from, const char *to, int is_dir)
{
        if (sync_rename(&g.roots.items[root_i], from, to)) {
                /* Changes waiting under the old name are now at the new one */
                BT *dirty       = &g.dirty[root_i];
                Da(char *) keys = { 0 };
                if (bt_get(dirty, from)) Da_append(&keys, strdup(from));
                if (is_dir) {
                        BT *d;
                        for_bt_each(d, dirty)
                        {
                                if (inside(d->key, from)) Da_append(&keys, strdup(d->key));
                        }
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
        } else {
                mark(root_i, from, SYNC_DATA);
                mark(root_i, to, is_dir ? SYNC_TREE : SYNC_DATA);
        }
}

/* Both ssh sessions (sftp and the agent) share one connection, so there is
 * a single login: the first one becomes the master. The socket goes in a
 * private directory. */
static void
make_ssh_opts(void)
{
        static char path[PATH_MAX];
        const char *dir = getenv("XDG_RUNTIME_DIR");
        snprintf(path, sizeof path, "-oControlPath=%s/isf-%%C", dir && *dir ? dir : "~/.ssh");
        g.ssh_opts[0] = "-oControlMaster=auto";
        g.ssh_opts[1] = path;
        g.ssh_opts[2] = "-oControlPersist=no";
        g.ssh_opts[3] = NULL;
}

/* "N thing" or "N things" */
static void
print_count(const char *sep, int n, const char *one, const char *many)
{
        printf("%s%d %s", sep, n, n == 1 ? one : many);
}

static void
print_summary(void)
{
        SyncStats st = sync_stats();
        if (st.sent + st.received + st.conflicts == 0) {
                printf("isf: already in sync\n");
                return;
        }
        if (g.dry_run) {
                printf("isf: dry run, nothing was changed:");
                print_count(" ", st.sent, "to send", "to send");
                print_count(", ", st.received, "to receive", "to receive");
        } else {
                printf("isf: in sync:");
                print_count(" ", st.sent, "sent", "sent");
                print_count(", ", st.received, "received", "received");
        }
        if (st.conflicts) print_count(", ", st.conflicts, "conflict", "conflicts");
        printf("\n");
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
        /* In an AppImage, /proc/self/exe is inside its mount, gone when isf
         * exits: the AppImage itself is the file to copy */
        char self[PATH_MAX];
        ssize_t n            = readlink("/proc/self/exe", self, sizeof self - 1);
        self[n > 0 ? n : 0]  = 0;
        const char *appimage = getenv("APPIMAGE");
        if (appimage && *appimage) snprintf(self, sizeof self, "%s", appimage);
        fprintf(stderr,
                "isf: isf has to be installed on '%s' too, and it isn't (or it isn't in the PATH of ssh\n"
                "     commands there). Copy it, for example:\n"
                "         scp %s%s%s%s %s:.local/bin/isf\n"
                "     and if ~/.local/bin isn't in that PATH, say where it is (it's remembered):\n"
                "         isf %s %s:%s -I .local/bin/isf\n",
                g.host, g.port ? "-P " : "", g.port ? g.port : "", g.port ? " " : "", *self ? self : "isf", g.host,
                g.roots.items[0].local, g.host, g.roots.items[0].remote);
}

/* A change the agent reported on the remote */
static void
remote_change(char type, int root, const char *rel)
{
        if (root >= g.roots.count) return;
        if (type == 'O') {
                mark(root, "", SYNC_TREE);
                return;
        }
        /* The remote could name a path that leaves the folder (../..): never
         * let it build a local path */
        if (!path_safe(rel)) {
                LOG_WARN("Ignoring an unsafe path from the remote: '%s'", rel);
                return;
        }
        /* 'C', or 'D' if it's a directory */
        if (!ignored(&g.roots.items[root].ign, rel, type == 'D')) mark(root, rel, SYNC_DATA);
}

static void
handle_event(const struct inotify_event *event, int fd)
{
        if (event->mask & IN_Q_OVERFLOW) {
                LOG_WARN("Event queue overflowed, some events were lost: checking everything");
                moved_out();
                for (int i = 0; i < g.roots.count; i++)
                        mark(i, "", SYNC_TREE);
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
                if (event->len) mark(root_i, rel, SYNC_ATTR);
        }

        if (event->mask & IN_CLOSE_WRITE) { // File opened for writing was closed.
                VPRINT("IN_CLOSE_WRITE ");
                mark(root_i, rel, SYNC_DATA);
        }

        if (event->mask & IN_CLOSE_NOWRITE) { // File or directory not opened for writing was closed.
                VPRINT("IN_CLOSE_NOWRITE ");
        }

        if (event->mask & IN_CREATE) { // File/directory created in watched directory (e.g., open(2) O_CREAT, mkdir(2),  link(2),  symlink(2),  bind(2)  on  a  UNIX  domain socket).
                VPRINT("IN_CREATE ");
                /* Watch a new directory first, then send all of it: things
                 * created in it before the watch have no events */
                if (is_dir) listen_folder(path, root_i, fd);
                mark(root_i, rel, is_dir ? SYNC_TREE : SYNC_DATA);
        }

        if (event->mask & IN_DELETE) { // File/directory deleted from watched directory.
                VPRINT("IN_DELETE ");
                mark(root_i, rel, SYNC_DATA);
        }

        if (event->mask & IN_DELETE_SELF) { // Watched  file/directory  was  itself deleted.  (This event also occurs if an object is moved to another filesystem, since mv(1) in effect copies the file to the other filesystem and then deletes it from the original  filesystem.)   In  addition,  an  IN_IGNORED event will subsequently be generated for the watch descriptor.
                VPRINT("IN_DELETE_SELF ");
                if (is_root) LOG_WARN("'%s' was removed, it's not synced anymore", path);
        }

        if (event->mask & IN_MODIFY) { // File was modified (e.g., write(2), truncate(2)).
                VPRINT("IN_MODIFY ");  // IN_CLOSE_WRITE follows, the file is sent then
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
                        /* Moved in from outside, or from another root */
                        moved_out();
                        mark(root_i, rel, is_dir ? SYNC_TREE : SYNC_DATA);
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

int
poll_watch_fds(watch_pollfds fds)
{
        for (;;) {
                /* With changes waiting, wait only DEBOUNCE_MS for more */
                int pending  = g.dirty_count > 0 || g.move.active;
                int poll_num = poll(fds.items, fds.count, pending ? DEBOUNCE_MS : -1);
                if (poll_num == -1) {
                        if (errno == EINTR) continue;
                        LOG_ERR("poll");
                        return 1;
                }

                if (poll_num > 0) {
                        Da_foreach(fd, fds)
                        {
                                if (fd->fd == g.remote.from && fd->revents) {
                                        if (agent_read(&g.remote, remote_change)) {
                                                LOG_WARN("The agent on the remote exited, stopping");
                                                return 1;
                                        }
                                } else if (fd->revents & POLLIN) {
                                        /* Inotify events are available.  */
                                        if (handle_events(fd->fd, handle_event)) return 1;
                                }
                        }
                }

                if (poll_num == 0) {
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
        flag_add(&g.reset, "--reset", .help = "forget what was synced before: sync like the first time");
        flag_add(&g.jobs_flag, "--jobs", "-j", .nargs = 1, .defaults = "4",
                 .help = "how many files to transfer at once (parallel connections)");
        flag_add(&g.dry_run, "--dry-run", "-n", .help = "show what syncing would do, change nothing, and exit");
        flag_add(&g.version, "--version", "-V", .help = "show the version");

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
        if (parse_args(argc, argv)) return 1;
        g.dirty = calloc(g.roots.count, sizeof *g.dirty);
        assert(g.dirty);
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
        sync_init(&g.sftp, g.host, g.port, g.ssh_opts, g.jobs, g.dry_run != NULL);
        Da_foreach(r, g.roots)
        {
                if (g.save_dest && !g.dry_run)
                        dest_save(r->local, &(Dest) { (char *) g.host, (char *) g.port, r->remote, (char *) g.isf });
        }

        Da_foreach(r, g.roots)
        {
                if (!g.dry_run && sftp_mkdir_p(&g.sftp, r->remote)) {
                        LOG_SFTP("Cannot create '%s:%s'", g.host, r->remote);
                        return 1;
                }
                printf("isf: %s ⇄ %s:%s\n", r->local, g.host, r->remote);
                if (g.roots.count > 1) r->label = r->local;
        }

        Da_foreach(r, g.roots)
        {
                if (sync_open(r, g.reset != NULL)) return 1;
        }

        if (g.dry_run) {
                Da_foreach(r, g.roots)
                {
                        reconcile(r, "", SYNC_TREE, NULL);
                }
                print_summary();
                sftp_disconnect(&g.sftp);
                flag_free();
                return 0;
        }

        /* Changes on the remote are watched from before the first sync */
        if (agent_start(&g.remote, g.isf ? g.isf : "isf", g.host, g.port, g.ssh_opts, g.roots.items, g.roots.count)) return 1;
        while (!g.remote.ready) {
                if (agent_read(&g.remote, remote_change)) {
                        agent_didnt_start(agent_stop(&g.remote));
                        return 1;
                }
        }

        int fd = watch_init();
        if (fd < 0) return 1;
        g.fd = fd;

        for (int i = 0; i < g.roots.count; i++) {
                if (listen_folder(g.roots.items[i].local, i, fd)) return 1;
        }

        /* Bring both sides together. After the watches, so what changes
         * meanwhile isn't lost: its events come after this. */
        for (int i = 0; i < g.roots.count; i++)
                mark(i, "", SYNC_TREE);
        flush();
        print_summary();
        printf("isf: watching for changes, Ctrl-C to stop\n");

        watch_pollfds fds = { 0 };
        Da_append(&fds, (struct pollfd) {
                        .fd     = fd,
                        .events = POLLIN,
                        });
        Da_append(&fds, (struct pollfd) { .fd = g.remote.from, .events = POLLIN });

        int ret = poll_watch_fds(fds);
        agent_stop(&g.remote);
        sync_shutdown();
        sftp_disconnect(&g.sftp);
        flag_free();
        return ret;
}
