#define _DEFAULT_SOURCE

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "agent.h"
#include "bt.h"
#include "ignore.h"
#include "sftp.h"
#include "watch.h"

/* How often a folder on a network filesystem is looked at, at least (see
 * poll_scan) */
#define POLL_MS 5000

/* A folder on a network filesystem: what changes in it from another machine
 * (a cluster's compute nodes writing to an NFS home) is no inotify event here,
 * so it is looked for by walking the folder every so often */
typedef struct {
        int on;
        BT snap;   // rel -> what was last seen or reported of it (snap_text)
        long next; // when to look again (now_ms)
} Poll;

/* Remote side: the folders it watches */
static struct {
        Root *roots;
        int count;
        /* The cookies of the last renames from isf's temp files: their
         * MOVED_TO is isf writing a file */
        uint32_t temp_moves[64];
        int temp_count;
        /* A MOVED_FROM waiting for its MOVED_TO: together they're a rename */
        struct {
                int active;
                uint32_t cookie;
                int root;
                char *path;
                int is_dir;
        } move;
        int fd; // inotify's
        Poll *poll;  // one per folder
        int poll_ms; // how often, at least
        int whole;   // the walk under way could look at everything
} g;

static long
now_ms(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Is COOKIE a rename from one of isf's temp files? (Forgotten then.) */
static int
temp_moved(uint32_t cookie)
{
        for (int i = 0; i < 64 && i < g.temp_count; i++) {
                if (g.temp_moves[i] == cookie && cookie) {
                        g.temp_moves[i] = 0;
                        return 1;
                }
        }
        return 0;
}

/* ST as text: size, mtime and mode in hex. 32 characters and a NUL into BUF. */
static void
stat_text(const struct stat *st, char *buf)
{
        sprintf(buf, "%016llx%08x%08x", (unsigned long long) st->st_size, (unsigned) st->st_mtime, (unsigned) st->st_mode);
}

/* What lstat says of PATH, as text (all 0 if it's gone) */
static void
lstat_text(const char *path, char *buf)
{
        struct stat st;
        if (lstat(path, &st) == -1) memset(&st, 0, sizeof st);
        stat_text(&st, buf);
}

/* What HEX says (the local side's, further down) */
static int hex(const char *s, int n, uint64_t *v);

/* ST as the polling snapshot keeps it: stat_text, but a directory's size and
 * mtime left out. They change with what's in it, which is compared on its own,
 * and isf's own uploads would make them differ. */
static void
snap_text(const struct stat *st, char *buf)
{
        struct stat s = *st;
        if (S_ISDIR(s.st_mode)) {
                s.st_size  = 0;
                s.st_mtime = 0;
        }
        stat_text(&s, buf);
}

/* Is TEXT (stat_text) a directory's? */
static int
text_is_dir(const char *text)
{
        uint64_t mode = 0;
        return hex(text + 24, 8, &mode) && S_ISDIR(mode);
}

/* REL is TEXT now, in SNAP */
static void
snap_put(BT *snap, const char *rel, const char *text)
{
        char *old = bt_get(snap, rel);
        if (old) {
                memcpy(old, text, 33);
                return;
        }
        char *copy = strdup(text);
        assert(copy);
        bt_add(snap, rel, copy);
}

/* REL is gone from SNAP, and what was in it if it's a directory */
static void
snap_forget(BT *snap, const char *rel)
{
        char *old = bt_get(snap, rel);
        if (old == NULL) return;
        int dir = text_is_dir(old);
        free(old);
        bt_del(snap, rel);
        if (!dir) return;

        /* Copies: deleting moves keys between nodes */
        size_t n = strlen(rel);
        Da(char *) below = { 0 };
        BT *e;
        for_bt_each(e, snap)
        {
                if (!strncmp(e->key, rel, n) && e->key[n] == '/') Da_append(&below, strdup(e->key));
        }
        Da_foreach(k, below)
        {
                free(bt_get(snap, *k));
                bt_del(snap, *k);
                free(*k);
        }
        Da_destroy(&below);
}

/* Put what's under PATH, a folder in ROOT_I, into SNAP: everything but temp
 * files and what's ignored. A walk that couldn't look at something clears
 * g.whole: what it missed isn't gone. */
static void
snap_walk(int root_i, const char *path, BT *snap)
{
        DIR *dir = opendir(path);
        if (dir == NULL) {
                if (errno != ENOENT) g.whole = 0;
                return;
        }
        const Root *root = &g.roots[root_i];
        struct dirent *e;
        while ((e = readdir(dir))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || is_temp_name(e->d_name)) continue;
                char *child = (char *) pathjoin(path, e->d_name);
                const char *rel = rel_path(root->local, child);
                struct stat st;
                if (lstat(child, &st) == -1) {
                        if (errno != ENOENT) g.whole = 0; // gone meanwhile is fine
                } else if (!ignored(&root->ign, rel, S_ISDIR(st.st_mode))) {
                        char text[33];
                        snap_text(&st, text);
                        snap_put(snap, rel, text);
                        if (S_ISDIR(st.st_mode)) snap_walk(root_i, child, snap);
                }
                free(child);
        }
        closedir(dir);
}

/* PATH (REL in ROOT_I) was reported as ST (all 0: gone): the next walk
 * compares against that, so nothing is reported twice */
static void
snap_note(int root_i, const char *path, const char *rel, const struct stat *st)
{
        if (g.poll == NULL || !g.poll[root_i].on || !*rel) return;
        BT *snap = &g.poll[root_i].snap;
        if (st->st_mode == 0) {
                snap_forget(snap, rel);
                return;
        }
        char text[33];
        snap_text(st, text);
        const char *old = bt_get(snap, rel);
        int was_dir     = old && text_is_dir(old);
        snap_put(snap, rel, text);
        /* A folder new here: what's in it has no events of its own */
        if (S_ISDIR(st->st_mode) && !was_dir) snap_walk(root_i, path, snap);
}

/* Tell the other side: TYPE, ROOT, VALUE and TEXT (see agent.h) */
static void
agent_send(char type, int root, uint64_t value, const char *text)
{
        size_t len = strlen(text) + 19;
        char *msg  = malloc(len);
        assert(msg);
        msg[0] = type;
        msg[1] = root;
        snprintf(msg + 2, 17, "%016llx", (unsigned long long) value);
        memcpy(msg + 18, text, len - 18);
        for (size_t done = 0; done < len;) {
                ssize_t n = write(STDOUT_FILENO, msg + done, len - done);
                if (n == -1 && errno == EINTR) continue;
                if (n == -1) exit(0); // the other side is gone
                done += n;
        }
        free(msg);
}

/* Report the change at PATH, in ROOT: KIND (see agent.h) and its lstat */
static void
agent_change(char kind, int root, const char *path)
{
        const char *rel = rel_path(g.roots[root].local, path);
        char *text      = malloc(strlen(rel) + 33);
        assert(text);
        struct stat st;
        if (lstat(path, &st) == -1) memset(&st, 0, sizeof st);
        stat_text(&st, text);
        snap_note(root, path, rel, &st);
        strcat(text, rel);
        agent_send(kind, root, 0, text);
        free(text);
}

/* The MOVED_FROM waiting got no MOVED_TO: it left the folders, which is a
 * removal */
static void
move_flush(void)
{
        if (!g.move.active) return;
        agent_change(g.move.is_dir ? 'D' : 'C', g.move.root, g.move.path);
        /* Its watches would keep reporting it under the old path */
        if (g.move.is_dir) unwatch(g.move.path, g.fd);
        free(g.move.path);
        g.move.active = 0;
}

/* Entries listed at the start, at most: past that, the other side lists the
 * rest itself (the tests build with a small one) */
#ifndef LIST_MAX
#define LIST_MAX 100000
#endif
/* A listing goes in messages of about this size */
#define LIST_CHUNK 65536

static void
append(CharBuf *b, const char *s, size_t n)
{
        for (size_t i = 0; i < n; i++)
                Da_append(b, s[i]);
}

/* Send what's in PATH, a folder in ROOT_I, then do the same for the folders in
 * it that aren't ignored, while *LEFT (entries) lasts: 'L' (see agent.h). Once
 * everything is watched, so what changes after it's listed is reported. */
static void
list_folder(int root_i, const char *path, long *left)
{
        if (*left <= 0) return;
        DIR *dir = opendir(path);
        if (dir == NULL) return; // listed over SFTP then, which says why
        const Root *root = &g.roots[root_i];
        const char *rel  = rel_path(root->local, path);
        char text[33];
        CharBuf msg = { 0 };
        lstat_text(path, text);
        append(&msg, text, 32);
        append(&msg, rel, strlen(rel));
        size_t head = msg.count; // the same in each message
        Da(char *) subdirs = { 0 };

        struct dirent *e;
        int whole = 1; // everything in it could be looked at
        while ((e = readdir(dir))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char *child = (char *) pathjoin(path, e->d_name);
                struct stat st;
                if (lstat(child, &st) == -1) {
                        free(child);
                        if (errno == ENOENT) continue; // gone meanwhile
                        /* A folder that can be read but not looked into (no
                         * x): what's in it would look like nothing, and the
                         * other side would take it for empty. Say nothing,
                         * and let it list the folder itself. */
                        whole = 0;
                        break;
                }
                size_t len = strlen(e->d_name);
                if ((size_t) msg.count + 32 + len + 1 > LIST_CHUNK && (size_t) msg.count > head) {
                        Da_append(&msg, 0);
                        agent_send('L', root_i, strlen(rel), msg.items);
                        msg.count = head;
                }
                stat_text(&st, text);
                append(&msg, text, 32);
                append(&msg, e->d_name, len);
                Da_append(&msg, '/');
                (*left)--;
                if (S_ISDIR(st.st_mode) && !ignored(&root->ign, rel_path(root->local, child), 1))
                        Da_append(&subdirs, child);
                else
                        free(child);
        }
        closedir(dir);
        if (whole) {
                Da_append(&msg, 0);
                agent_send('L', root_i, strlen(rel), msg.items);
        }
        Da_destroy(&msg);

        Da_foreach(p, subdirs)
        {
                list_folder(root_i, *p, left);
                free(*p);
        }
        Da_destroy(&subdirs);
}

/* An ignored folder isn't watched here either: it would only use watches, of
 * which a small machine hasn't many, for events the other side drops. */
static int
skip_watch(int root, const char *path)
{
        const char *rel = rel_path(g.roots[root].local, path);
        return *rel && ignored(&g.roots[root].ign, rel, 1);
}

/* Put TMP in the place of TARGET (both inside ROOT), if TARGET is still what
 * EXPECT says (an lstat in hex; all 0: nothing is there). One lstat and one
 * rename, here, so nothing can change in between. Answers 'p' (see agent.h):
 * 'o' done, 'c' it changed there (nothing moved, TMP dropped), 'e' it
 * couldn't, and then TMP is left for the other side to try over SFTP. */
static void
place(int root_i, uint64_t id, const char *expect, const char *tmp_rel, const char *target_rel)
{
        const char *name = strrchr(tmp_rel, '/');
        char why[160]    = "";
        char how         = 'e';

        if (!path_safe(tmp_rel) || !path_safe(target_rel) || !*tmp_rel || !*target_rel ||
            !is_temp_name(name ? name + 1 : tmp_rel)) {
                LOG_WARN("Not putting '%s' in the place of '%s': that isn't what isf does", tmp_rel, target_rel);
                agent_send('p', root_i, id, "e");
                return;
        }
        const char *tmp    = root_join(g.roots[root_i].local, tmp_rel);
        const char *target = root_join(g.roots[root_i].local, target_rel);

        struct stat st;
        char now[33] = "00000000000000000000000000000000"; // nothing there
        if (lstat(target, &st) == 0)
                stat_text(&st, now);
        else if (errno != ENOENT)
                snprintf(why, sizeof why, "%s", strerror(errno));

        if (*why) {
                how = 'e';
        } else if (strcmp(now, expect)) {
                /* Changed since the other side looked: left as it is, and
                 * what was on its way goes away */
                how = 'c';
                unlink(tmp);
        } else if (rename(tmp, target) == 0) {
                how = 'o';
        } else {
                /* Something unusual (a directory in the way): the other side
                 * tries it over SFTP, so TMP stays */
                snprintf(why, sizeof why, "%s", strerror(errno));
        }
        agent_send('p', root_i, id, (char[]) { how, 0 });
        /* Not a warning: the other side tries it over SFTP, and says so if
         * that fails too */
        if (how == 'e') VPRINT("File: %s [cannot be put in place here: %s]\n", target_rel, why);
        free((void *) tmp);
        free((void *) target);
}

/* Read what the other side asks (its 'P' requests) and answer. Returns 1 when
 * it's gone (its end of the pipe closed), which stops the agent. */
static int
read_requests(void)
{
        static CharBuf buf; // what's read, but not a whole message yet
        char chunk[4096];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n == -1 && errno == EINTR) return 0;
        if (n <= 0) return 1;
        for (ssize_t i = 0; i < n; i++)
                Da_append(&buf, chunk[i]);

        /* Like the messages it sends: type, folder, number, text, NUL */
        size_t start = 0;
        while (buf.count - start >= 3) {
                char *nul = memchr(buf.items + start + 2, 0, buf.count - start - 2);
                if (nul == NULL) break;
                char type        = buf.items[start];
                int root         = (unsigned char) buf.items[start + 1];
                const char *text = buf.items + start + 2;
                uint64_t id      = 0;
                start            = nul - buf.items + 1;
                if (type != 'P' || root >= g.count || nul - text < 48 || !hex(text, 16, &id)) continue;

                /* The lstat it has to still have, the temp file's path (as
                 * long as the 8 digits say) and the target's */
                uint64_t len = 0;
                if (!hex(text + 48, 8, &len) || len > (uint64_t) (nul - text - 56)) continue;
                char expect[33];
                memcpy(expect, text + 16, 32);
                expect[32]        = 0;
                const char *paths = text + 56;
                char *tmp         = strndup(paths, len);
                assert(tmp);
                place(root, id, expect, tmp, paths + len);
                free(tmp);
        }
        memmove(buf.items, buf.items + start, buf.count - start);
        buf.count -= start;
        return 0;
}

/* A file written to without being closed (a log), now that the writes
 * stopped for a while: someone else's write */
static void
held_remote(const char *path, int root)
{
        agent_change('C', root, path);
}

/* The agent's handle_event: report what changed, don't sync anything */
static void
agent_event(const struct inotify_event *event, int fd)
{
        /* A MOVED_FROM is followed by its MOVED_TO, if the thing stays in the
         * folders. Anything else first: it left. */
        if (g.move.active && !((event->mask & IN_MOVED_TO) && event->cookie == g.move.cookie)) move_flush();

        if (event->mask & IN_Q_OVERFLOW) {
                for (int i = 0; i < g.count; i++)
                        agent_send('O', i, 0, "");
                return;
        }

        Watch *w = find_watch(event->wd);
        if (w == NULL) return;
        int root_i       = w->root;
        const Root *root = &g.roots[root_i];

        if (event->mask & IN_IGNORED) {
                forget_watch(w);
                return;
        }
        if (event->len == 0) {
                /* Without its folder the other side would see everything as
                 * deleted: stop, so it stops too */
                if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) && !strcmp(w->path, root->local)) {
                        LOG_WARN("'%s' was removed or moved, stopping", w->path);
                        exit(1);
                }
                return;
        }
        if (is_temp_name(event->name)) {
                /* A transfer in progress: not said. Its rename into place is. */
                if (event->mask & IN_MOVED_FROM) g.temp_moves[g.temp_count++ % 64] = event->cookie;
                return;
        }

        const char *path = pathjoin(w->path, event->name);
        if ((event->mask & (IN_CREATE | IN_MOVED_TO)) && (event->mask & IN_ISDIR))
                listen_folder(path, root_i, fd);
        /* Written to and not closed (a log): said once the writes stop */
        if ((event->mask & IN_MODIFY) && !(event->mask & IN_ISDIR)) held_write(path, root_i);
        if (event->mask & (IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM)) held_forget(path);
        if (event->mask & IN_MOVED_FROM) {
                /* Wait for its MOVED_TO */
                g.move.active = 1;
                g.move.cookie = event->cookie;
                g.move.root   = root_i;
                g.move.path   = strdup(path);
                g.move.is_dir = (event->mask & IN_ISDIR) != 0;
        } else if ((event->mask & IN_MOVED_TO) && g.move.active && g.move.root == root_i) {
                /* A rename inside the folder: the other side can do the same */
                const char *from = rel_path(root->local, g.move.path);
                const char *to   = rel_path(root->local, path);
                char *text       = malloc(strlen(from) + strlen(to) + 33);
                assert(text);
                struct stat st;
                if (lstat(path, &st) == -1) memset(&st, 0, sizeof st);
                stat_text(&st, text);
                if (g.poll && g.poll[root_i].on) snap_forget(&g.poll[root_i].snap, from);
                snap_note(root_i, path, to, &st);
                strcat(strcat(text, from), to);
                agent_send('M', root_i, strlen(from), text);
                free(text);
                free(g.move.path);
                g.move.active = 0;
        } else if (event->mask & (IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_TO)) {
                /* The patterns changed: what they don't skip anymore is
                 * watched now (the other side syncs the file itself) */
                if (!strcmp(event->name, IGNORE_FILE) && !strcmp(w->path, root->local)) {
                        ignore_load(&g.roots[root_i].ign, root->local);
                        listen_folder(root->local, root_i, fd);
                }
                /* Who wrote it matters: a write by someone else changes it,
                 * whatever its size and mtime (whole seconds) say */
                char kind = event->mask & IN_ISDIR                                   ? 'D' :
                            event->mask & IN_MOVED_TO && temp_moved(event->cookie) ? 'W' :
                            event->mask & IN_ATTRIB                                  ? 'A' :
                                                                                       'C';
                agent_change(kind, root_i, path);
        }
        free((void *) path);
}

/* Is PATH on a filesystem that other machines write to directly? Their
 * changes reach this kernel without going through it, so inotify never hears
 * of them. FUSE is left out: most of it is local. */
static int
on_network_fs(const char *path)
{
        struct statfs sf;
        if (statfs(path, &sf) == -1) return 0;
        switch ((uint32_t) sf.f_type) {
        case 0x6969:     // NFS
        case 0x517B:     // SMB
        case 0xFF534D42: // CIFS
        case 0xFE534D42: // SMB2
        case 0x0BD00BD0: // Lustre
        case 0x47504653: // GPFS
        case 0x00C36400: // Ceph
        case 0x5346414F: // AFS
        case 0x19830326: // BeeGFS
        case 0x01021997: // 9p
                return 1;
        }
        return 0;
}

/* A path the walk found different from what was last seen or reported:
 * TEXT is what it is now (unused if GONE) */
typedef struct {
        char *rel;
        char text[33];
        int gone;
} Found;

/* Walk folder ROOT_I and report what changed since the last walk that no
 * event reported: what other machines did. */
static void
poll_scan(int root_i)
{
        Poll *p          = &g.poll[root_i];
        const Root *root = &g.roots[root_i];
        BT now           = { 0 };
        g.whole          = 1;
        snap_walk(root_i, root->local, &now);

        Da(Found) found = { 0 };
        BT *e;
        for_bt_each(e, &now)
        {
                const char *old = bt_get(&p->snap, e->key);
                if (old && !strcmp(old, e->value)) continue;
                Found f = { .rel = strdup(e->key) };
                memcpy(f.text, e->value, 33);
                Da_append(&found, f);
        }
        /* A walk that missed something would take it for gone */
        if (g.whole) {
                for_bt_each(e, &p->snap)
                {
                        if (!bt_get(&now, e->key)) Da_append(&found, ((Found) { .rel = strdup(e->key), .gone = 1 }));
                }
        }
        for_bt_each(e, &now)
        {
                free(e->value);
        }
        bt_destroy(&now);

        /* A change made on this machine, isf's own uploads included, was
         * queued as an event before the walk could see it: those say who did
         * it, so they go first, and what they report isn't reported again */
        handle_events(g.fd, agent_event);

        Da_foreach(f, found)
        {
                const char *old = bt_get(&p->snap, f->rel);
                int reported    = f->gone ? old == NULL : old && !strcmp(old, f->text);
                if (!reported) {
                        const char *path = root_join(root->local, f->rel);
                        int dir          = (old && text_is_dir(old)) || (!f->gone && text_is_dir(f->text));
                        /* Only the mode changed: attributes, not a write */
                        int attrs = old && !f->gone && !dir && !strncmp(old, f->text, 24);
                        if (dir && !f->gone) listen_folder(path, root_i, g.fd);
                        agent_change(dir ? 'D' : attrs ? 'A' : 'C', root_i, path);
                        if (!strcmp(f->rel, IGNORE_FILE)) {
                                ignore_load(&g.roots[root_i].ign, root->local);
                                listen_folder(root->local, root_i, g.fd);
                        }
                        free((void *) path);
                }
                free(f->rel);
        }
        Da_destroy(&found);
}

/* Milliseconds until a folder is due to be walked, -1 if none is polled */
static int
poll_timeout(void)
{
        long wait = -1, now = now_ms();
        for (int i = 0; i < g.count; i++) {
                if (!g.poll[i].on) continue;
                long left = g.poll[i].next > now ? g.poll[i].next - now : 0;
                if (wait < 0 || left < wait) wait = left;
        }
        return (int) wait;
}

/* Walk the folders that are due. A slow walk (a big tree on a busy server)
 * waits ten times as long as it took before the next one. */
static void
poll_due(void)
{
        for (int i = 0; i < g.count; i++) {
                long start = now_ms();
                if (!g.poll[i].on || start < g.poll[i].next) continue;
                poll_scan(i);
                long took      = now_ms() - start;
                g.poll[i].next = now_ms() + (10 * took > g.poll_ms ? 10 * took : g.poll_ms);
        }
}

int
agent_main(Root *roots, int count)
{
        g.roots = roots;
        g.count = count;

        int fd = watch_init();
        if (fd < 0) return 1;
        g.fd = fd;
        for (int i = 0; i < count; i++) {
                /* Made if missing: the first sync of a folder there */
                if (mkdir_p(roots[i].local) == -1) {
                        LOG_ERR("Cannot create '%s'", roots[i].local);
                        return 1;
                }
                ignore_load(&roots[i].ign, roots[i].local);
        }
        watch_skip(skip_watch);
        for (int i = 0; i < count; i++) {
                if (listen_folder(roots[i].local, i, fd)) return 1;
        }
        /* Folders other machines write to are also walked every so often.
         * ISF_TEST_POLL_MS walks every folder, that often (the tests' folders
         * aren't on one). Their first snapshot comes before the listings, so
         * a change made in between is found by the first walk. */
        const char *test_poll = getenv("ISF_TEST_POLL_MS");
        g.poll_ms             = test_poll && atoi(test_poll) > 0 ? atoi(test_poll) : POLL_MS;
        g.poll                = calloc(count, sizeof *g.poll);
        assert(g.poll);
        for (int i = 0; i < count; i++) {
                if (!(test_poll && atoi(test_poll) > 0) && !on_network_fs(roots[i].local)) continue;
                g.poll[i].on   = 1;
                g.poll[i].next = now_ms() + g.poll_ms;
                snap_walk(i, roots[i].local, &g.poll[i].snap);
                VPRINT("'%s' is on a network filesystem: looking for changes made from other machines every %d ms\n",
                       roots[i].local, g.poll_ms);
        }
        /* What's there now, for the first walk of the other side. Not what
         * its .isfignore here ignores: the other side lists that itself, if
         * its own .isfignore doesn't. */
        long left = LIST_MAX;
        for (int i = 0; i < count; i++)
                list_folder(i, roots[i].local, &left);
        agent_send('T', 0, (uint64_t) time(NULL), ""); // to tell the clocks apart
        agent_send('R', 0, AGENT_PROTOCOL, VERSION);

        struct pollfd fds[] = {
                { .fd = fd, .events = POLLIN },
                { .fd = STDIN_FILENO, .events = POLLIN },
        };
        for (;;) {
                /* A MOVED_FROM waits for its MOVED_TO only so long, a file
                 * held open until it's due, and a polled folder until it's
                 * walked again */
                int wait    = g.move.active ? 50 : -1;
                int held    = held_timeout();
                int walk    = poll_timeout();
                int timeout = wait;
                if (held >= 0 && (timeout < 0 || held < timeout)) timeout = held;
                if (walk >= 0 && (timeout < 0 || walk < timeout)) timeout = walk;
                int n = poll(fds, 2, timeout);
                if (n == -1) {
                        if (errno == EINTR) continue;
                        LOG_ERR("poll");
                        return 1;
                }
                if (n == 0 && wait >= 0 && timeout == wait) move_flush();
                held_due(held_remote);
                if ((fds[1].revents & POLLIN) && read_requests()) return 0; // it's gone
                if (fds[1].revents & (POLLHUP | POLLERR)) return 0;
                if ((fds[0].revents & POLLIN) && handle_events(fd, agent_event)) return 1;
                poll_due();
        }
}

/* Append S to B in single quotes, for the remote shell */
static void
append_quoted(CharBuf *b, const char *s)
{
        Da_append(b, '\'');
        for (; *s; s++) {
                if (*s == '\'') {
                        for (const char *q = "'\\''"; *q; q++)
                                Da_append(b, *q);
                } else {
                        Da_append(b, *s);
                }
        }
        Da_append(b, '\'');
}

int
agent_start(Agent *a, const char *isf, const char *host, const char *port,
            char *const *ssh_opts, const Root *roots, int count)
{
        *a = (Agent) { .pid = -1, .to = -1, .from = -1 };

        /* ssh sends the command to the remote shell as one string */
        CharBuf cmd = { 0 };
        for (const char *p = isf; *p; p++)
                Da_append(&cmd, *p); // as given, so ~ still expands
        for (const char *p = " --agent"; *p; p++)
                Da_append(&cmd, *p);
        for (int i = 0; i < count; i++) {
                Da_append(&cmd, ' ');
                append_quoted(&cmd, roots[i].remote);
        }
        Da_append(&cmd, 0);

        Command c = { 0 };
        Command_add(&c, "ssh", "-x", "-a", "-oClearAllForwardings=yes", "-oPermitLocalCommand=no",
                    "-oRemoteCommand=none", "-oRequestTTY=no");
        for (; ssh_opts && *ssh_opts; ssh_opts++)
                Command_add(&c, *ssh_opts);
        if (port) Command_add(&c, "-p", port);
        Command_add(&c, "--", host, cmd.items, NULL);

        a->pid = spawn_piped(c.items, &a->to, &a->from);
        Command_destroy(&c);
        Da_destroy(&cmd);
        if (a->pid == -1) {
                LOG_ERR("Cannot run ssh");
                return 1;
        }
        return 0;
}

/* The N hex digits at S, into *V. Returns 0 if they aren't. */
static int
hex(const char *s, int n, uint64_t *v)
{
        *v = 0;
        for (int i = 0; i < n; i++) {
                int c = s[i];
                int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                if (d < 0) return 0;
                *v = *v << 4 | d;
        }
        return 1;
}

/* What lstat_text wrote at S, 32 hex digits */
static int
lstat_parse(const char *s, uint64_t *size, uint64_t *mtime, uint64_t *mode)
{
        return hex(s, 16, size) && hex(s + 16, 8, mtime) && hex(s + 24, 8, mode);
}

/* An 'L' message: TEXT (to NUL) is the folder's lstat and its path, LEN long,
 * then each entry's lstat, its name and a '/'. A damaged one goes to LISTED
 * with no DIR: some of the entries would look deleted. */
static void
read_listing(int root, uint64_t len, const char *text, const char *nul,
             void (*listed)(int root, const char *rel, const SftpAttrs *self, SftpDir *dir))
{
        uint64_t size, mtime, mode;
        const char *p = text + 32;
        if (nul - text < 32 || !lstat_parse(text, &size, &mtime, &mode) || len > (uint64_t) (nul - p)) {
                listed(root, "", NULL, NULL);
                return;
        }
        const SftpAttrs self = { .flags = SFTP_ATTR_SIZE | SFTP_ATTR_PERMISSIONS | SFTP_ATTR_ACMODTIME,
                                 .size  = size,
                                 .perm  = mode,
                                 .mtime = mtime };
        char *rel   = strndup(p, len);
        SftpDir dir = { 0 };
        int ok      = 1;
        for (p += len; ok && p < nul;) {
                const char *end = nul - p > 32 ? memchr(p + 32, '/', nul - p - 32) : NULL;
                ok              = end != NULL && lstat_parse(p, &size, &mtime, &mode);
                if (!ok) break;
                SftpEntry e = { .name  = strndup(p + 32, end - p - 32),
                                .attrs = { .flags = self.flags, .size = size, .perm = mode, .mtime = mtime } };
                Da_append(&dir, e);
                p = end + 1;
        }
        if (ok) {
                listed(root, rel, &self, &dir);
        } else {
                sftp_dir_free(&dir);
                listed(root, rel, NULL, NULL);
        }
        free(rel);
}

int
agent_place(Agent *a, int root, uint64_t id, const SftpAttrs *expect, const char *tmp, const char *target)
{
        char head[57];
        snprintf(head, sizeof head, "%016llx%016llx%08x%08x%08x", (unsigned long long) id,
                 (unsigned long long) (expect ? expect->size : 0), expect ? expect->mtime : 0,
                 expect ? expect->perm : 0, (unsigned) strlen(tmp));
        CharBuf msg = { 0 };
        Da_append(&msg, 'P');
        Da_append(&msg, (char) root);
        for (const char *p = head; *p; p++)
                Da_append(&msg, *p);
        for (const char *p = tmp; *p; p++)
                Da_append(&msg, *p);
        for (const char *p = target; *p; p++)
                Da_append(&msg, *p);
        Da_append(&msg, 0);

        int gone = 0;
        for (int done = 0; done < msg.count;) {
                ssize_t n = write(a->to, msg.items + done, msg.count - done);
                if (n == -1 && errno == EINTR) continue;
                if (n <= 0) {
                        gone = 1;
                        break;
                }
                done += n;
        }
        Da_destroy(&msg);
        return gone;
}

int
agent_read(Agent *a, void (*handle)(char type, int root, const char *rel, const State *seen),
           void (*moved)(int root, const char *from, const char *to, const State *seen),
           void (*listed)(int root, const char *rel, const SftpAttrs *self, SftpDir *dir),
           void (*placed)(uint64_t id, char how))
{
        char chunk[65536];
        ssize_t n = read(a->from, chunk, sizeof chunk);
        if (n == -1 && errno == EINTR) return 0;
        if (n <= 0) return 1;
        for (ssize_t i = 0; i < n; i++)
                Da_append(&a->buf, chunk[i]);

        /* A message is type, root, text and NUL: at least 3 bytes. The text
         * starts with the number, but not in protocol 1. */
        char *b      = a->buf.items;
        size_t start = 0, count = a->buf.count;
        while (count - start >= 3) {
                char *nul = memchr(b + start + 2, 0, count - start - 2);
                if (nul == NULL) break;
                char type        = b[start];
                int root         = (unsigned char) b[start + 1];
                const char *text = b + start + 2;
                uint64_t value   = 0;
                int numbered     = nul - text >= 16 && hex(text, 16, &value);
                start            = nul - b + 1;

                if (type == 'R') {
                        a->ready    = 1;
                        a->protocol = numbered ? (int) value : 1;
                        free(a->version);
                        a->version = numbered ? strdup(text + 16) : NULL;
                } else if (type == 'p' && numbered) {
                        placed(value, nul - text > 16 ? text[16] : 'e');
                } else if (type == 'T' && numbered) {
                        a->time = value;
                } else if (type == 'L' && numbered) { // before 'R', which says the protocol
                        read_listing(root, value, text + 16, nul, listed);
                } else if (a->protocol < 2) {
                        handle(type, root, text, NULL);
                } else if (numbered && strchr("CWADM", type)) {
                        /* What it is now, from its lstat */
                        uint64_t size, mtime, mode;
                        if (nul - text < 48 || !lstat_parse(text + 16, &size, &mtime, &mode)) continue;
                        State seen = {
                                .type  = !mode ? 0 : S_ISREG(mode) ? 'f' : S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '?',
                                .size  = size,
                                .mtime = mtime,
                                .mode  = mode & 07777,
                        };
                        if (type != 'M') {
                                handle(type, root, text + 48, &seen);
                                continue;
                        }
                        /* The old path (VALUE long), then the new one */
                        const char *paths = text + 48;
                        if (value > (uint64_t) (nul - paths)) continue;
                        char *from = strndup(paths, value);
                        moved(root, from, paths + value, &seen);
                        free(from);
                } else if (numbered) {
                        handle(type, root, text + 16, NULL);
                }
        }
        memmove(b, b + start, count - start);
        a->buf.count = count - start;
        return 0;
}

int
agent_stop(Agent *a)
{
        int status = -1;
        if (a->to != -1) close(a->to); // EOF on its stdin stops it
        if (a->from != -1) close(a->from);
        if (a->pid > 0 && waitpid(a->pid, &status, 0) > 0)
                status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        Da_destroy(&a->buf);
        free(a->version);
        *a = (Agent) { .pid = -1, .to = -1, .from = -1 };
        return status;
}
