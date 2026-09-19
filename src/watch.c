#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"
#include "watch.h"

static Da(Watch) watches; // maps event->wd back to a path, sorted by wd

int
watch_init(void)
{
        int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (fd < 0) LOG_ERR("inotify_init");
        return fd;
}

static int
watch_cmp(const void *a, const void *b)
{
        int x = ((const Watch *) a)->wd, y = ((const Watch *) b)->wd;
        return (x > y) - (x < y);
}

Watch *
find_watch(int wd)
{
        if (watches.count == 0) return NULL;
        Watch key = { .wd = wd };
        return bsearch(&key, watches.items, watches.count, sizeof key, watch_cmp);
}

void
forget_watch(Watch *w)
{
        free(w->path);
        Da_remove(&watches, Da_index(w, &watches));
}

/* Stop watching PATH and everything below it */
void
unwatch(const char *path, int fd)
{
        for (int i = watches.count - 1; i >= 0; i--) {
                Watch *w = &watches.items[i];
                if (strcmp(w->path, path) && !inside(w->path, path)) continue;
                inotify_rm_watch(fd, w->wd);
                free(w->path);
                Da_remove(&watches, i);
        }
}

int
handle_events(int fd, void (*handle)(const struct inotify_event *event, int fd))
{
        char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
        const struct inotify_event *event;
        ssize_t size;

        for (;;) {
                size = read(fd, buf, sizeof(buf));
                if (size == -1 && errno != EAGAIN) {
                        LOG_ERR("read");
                        return 1;
                }

                /* If the nonblocking read() found no events to read, then
                   it returns -1 with errno set to EAGAIN.  In that case,
                   we exit the loop.  */
                if (size <= 0) break;

                /* Loop over all events in the buffer.  */
                for (char *ptr = buf; ptr < buf + size;
                     ptr += sizeof(struct inotify_event) + event->len) {
                        event = (const struct inotify_event *) ptr;
                        handle(event, fd);
                }
        }

        return 0;
}

/* Out of inotify watches: say so once, with the way out. The folders left
 * unwatched are still synced when isf starts, but changes there aren't seen
 * while it runs. */
static void
watch_limit_reached(void)
{
        static int said;
        if (said++) return;
        char host[256] = "this machine";
        gethostname(host, sizeof host - 1);
        /* The limit of this user namespace (a container's), else the system's */
        long limit = 0;
        FILE *f    = fopen("/proc/sys/user/max_inotify_watches", "r");
        if (f == NULL) f = fopen("/proc/sys/fs/inotify/max_user_watches", "r");
        if (f) {
                if (fscanf(f, "%ld", &limit) != 1) limit = 0;
                fclose(f);
        }
        long more = limit < 524288 ? 524288 : 2 * limit;
        LOG_WARN("On '%s', the limit of inotify watches (%ld, one per folder, for all of this user's "
                 "programs) is reached: changes in some folders won't be seen until isf starts again. "
                 "Raise it there, then restart isf:\n"
                 "         sudo sysctl fs.inotify.max_user_watches=%ld\n"
                 "     and to keep it: echo fs.inotify.max_user_watches=%ld | sudo tee /etc/sysctl.d/90-isf.conf",
                 host, limit, more, more);
}

static int
watch_dir(const char *path, int root, int fd)
{
        int wd = inotify_add_watch(fd, path, WATCH_MASK);
        if (wd == -1) {
                if (errno == ENOSPC)
                        watch_limit_reached();
                else
                        LOG_ERR("Cannot watch '%s'", path);
                return 1;
        }

        /* Watching an already watched directory (e.g. after it was renamed)
         * returns the same wd, so just update its path. */
        Watch *w = find_watch(wd);
        if (w) {
                free(w->path);
                w->path = strdup(path);
                w->root = root;
                return 0;
        }

        /* Keep them sorted. New wds are usually the largest: an append. */
        int at = watches.count;
        while (at > 0 && watches.items[at - 1].wd > wd)
                at--;
        Watch n = { .wd = wd, .root = root, .path = strdup(path) };
        Da_insert(&watches, n, at);
        return 0;
}

int
listen_folder(const char *path, int root, int fd)
{
        if (watch_dir(path, root, fd)) return 1;

        DIR *dir = opendir(path);
        if (dir == NULL) {
                LOG_ERR("Cannot open '%s'", path);
                return 1;
        }

        struct dirent *entry;
        while ((entry = readdir(dir))) {
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

                const char *subpath = pathjoin(path, entry->d_name);
                int is_dir          = entry->d_type == DT_DIR;

                /* Some filesystems don't fill d_type. lstat so symlinks are
                 * not followed, same as DT_LNK. */
                if (entry->d_type == DT_UNKNOWN) {
                        struct stat st;
                        is_dir = lstat(subpath, &st) == 0 && S_ISDIR(st.st_mode);
                }

                /* Files are covered by this directory's watch. A failing
                 * subfolder is already logged and shouldn't stop the rest. */
                if (is_dir) listen_folder(subpath, root, fd);
                free((void *) subpath);
        }

        closedir(dir);
        return 0;
}
