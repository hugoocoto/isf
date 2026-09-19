#ifndef WATCH_H_
#define WATCH_H_

#include <sys/inotify.h>

/* Only directories are watched: a directory watch also reports these events for
 * the files inside it. Read-only events (IN_ACCESS, IN_OPEN, IN_CLOSE_NOWRITE)
 * are left out. */
#define WATCH_MASK (IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |        \
                    IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | \
                    IN_MOVED_TO | IN_ONLYDIR)

typedef struct watch {
        int wd;
        int root; // index of the folder it belongs to
        char *path;
} Watch;

/* Start inotify (nonblocking). Returns its fd, or -1 (logged). */
int watch_init(void);
/* Watch PATH and every directory below it, as part of folder ROOT */
int listen_folder(const char *path, int root, int fd);
/* The watch an event is for, NULL if it was removed already */
Watch *find_watch(int wd);
/* Forget W after its IN_IGNORED: the directory is gone */
void forget_watch(Watch *w);
/* Stop watching PATH and everything below it */
void unwatch(const char *path, int fd);
/* Read the events waiting on FD and call HANDLE for each. Returns 1 on error. */
int handle_events(int fd, void (*handle)(const struct inotify_event *event, int fd));

/* Files written to and not closed yet (a log): an IN_MODIFY without its
 * IN_CLOSE_WRITE. Each is due once the writes to it stop for HELD_QUIET_MS,
 * or HELD_MAX_MS after the first one if they don't. */
#define HELD_QUIET_MS 2000
#define HELD_MAX_MS 30000
/* PATH, in folder ROOT, was written to */
void held_write(const char *path, int root);
/* PATH was closed after writing, removed or moved: that says it */
void held_forget(const char *path);
/* Milliseconds until the next one is due, -1 if none */
int held_timeout(void);
/* Call FN for each one due, and forget them */
void held_due(void (*fn)(const char *path, int root));

#endif // !WATCH_H_
