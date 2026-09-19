#ifndef PLAN_H_
#define PLAN_H_

#include <stdint.h>

#include "cum.h"

/* Deciding what to do, apart from doing it: given what each side has and
 * what both had when they were last synced, the steps that bring them
 * together. Nothing here touches files or the connection. */

/* What the events said changed at a path. reconcile looks at what both sides
 * have at that moment; these only say how much to look at. */
enum {
        SYNC_ATTR = 1 << 0, // only metadata (chmod, touch)
        SYNC_DATA = 1 << 1, // content, or it was created or removed
        SYNC_TREE = 1 << 2, // new directory: everything inside too
};

/* A path's state on one side, or in the record: what both sides had when it
 * was last synced */
typedef struct {
        char type; // 0 missing, 'f' file, 'd' directory, 'l' symlink, '?' other
        uint64_t size;
        uint32_t mtime;
        uint32_t mode;   // permission bits
        char *link;      // symlink target
        uint64_t stamp;  // local files: ctime in ns, any change moves it
        int written;     // remote files: the agent said someone else wrote it since
                         // the last flush (not recorded)
} State;

State state_copy(const State *st);
void state_free(State *st);
/* Same content (and mode)? Directories only compare the mode. */
int same(const State *a, const State *b);
/* Did the local file L change since R was recorded? Also compares the ctime:
 * mtimes only have seconds, and an edit in the same second with the same
 * size would look the same. */
int same_local(const State *l, const State *r);
/* Did the remote file M change since R was recorded? Written by someone else,
 * it did, even with the same size and mtime (SFTP's are whole seconds). */
int same_remote(const State *m, const State *r);
/* Same file, maybe apart from the mode? Then only the mode has to be sent. */
int same_data(const State *a, const State *b);

/* One step. UP: it changes the remote, else the local side. */
typedef enum {
        ACT_COPY,   // copy the file or symlink over what the other side has
        ACT_ATTRS,  // copy only a file's mode (and times, up)
        ACT_REMOVE, // remove it: deleted on the other side
        ACT_MKDIR,  // make the directory, out of the way of what's there
        ACT_RMDIR,  // remove the directory the other side deleted, emptied by the
                    // steps before; if something is left in it, record ST
        ACT_MODE,   // set a directory's mode, once what goes inside is in
        ACT_RECORD, // both sides have ST already
} ActType;

typedef struct {
        ActType type;
        int up;
        char *rel;
        State L, M; // what the local side and the remote had when it was planned
        State st;   // what to record once it's done (COPY and ATTRS find it out)
        int conflict; // COPY: both sides changed it, this one is the newest. The
                      // other file (not a symlink) is kept as FILE.isf-conflict
        int keep;     // MKDIR: the file in the way changed, keep it the same way
        int quiet;    // not reported: MODE, part of making the directory; REMOVE,
                      // of a temp file left by an interrupted transfer
} Action;

typedef Da(Action) Plan;

void plan_free(Plan *plan);

/* Plan REL, which isn't a directory on either side. L, M and R are what the
 * local side, the remote and the record have. WHAT is the SYNC_* of the
 * change. Whichever side changed since the record wins; if both did, the
 * newest mtime, and an edit beats a deletion. */
void plan_file(Plan *plan, const char *rel, const State *L, const State *M, const State *R, int what);

/* REL is a directory on at least one side */
typedef enum {
        DIR_BOTH, // on both sides: fix the mode, go inside
        DIR_GONE, // the other side deleted it: go inside, where what changed
                  // since is kept, then remove it if nothing was
        DIR_LOST, // the other side replaced it with a file, and it didn't change
                  // since: the file wins
        DIR_WINS, // make it on the other side (new, or both changed: the
                  // directory wins over the file)
} DirCase;

DirCase decide_dir(const State *L, const State *M, const State *R);

/* DIR_BOTH: the mode both should have, and whether it goes UP. Returns 0 if
 * they have the same already. */
int dir_mode(const State *L, const State *M, const State *R, uint32_t *mode, int *up);

/* Append a step */
void plan_add(Plan *plan, ActType type, int up, const char *rel, const State *L, const State *M, const State *st);

#endif // !PLAN_H_
