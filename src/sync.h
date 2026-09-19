#ifndef SYNC_H_
#define SYNC_H_

#include <stdint.h>

#include "bt.h"
#include "ignore.h"
#include "plan.h"
#include "sftp.h"

/* Two-way sync of one path at a time. Each side's state is compared with the
 * record of the last synced state (plan.h decides), then the plan is done. */

typedef struct {
        char *rel;
        State st;
} Record;

/* A folder given on the command line */
typedef struct root {
        const char *local; // as given
        char *remote;      // -F folder joined with LOCAL
        Da(Record) rec;    // last synced state of everything, sorted by rel
        char *rec_path;    // where it's saved
        int rec_changed;   // not saved yet
        const char *label; // shown before its paths, when there are several
        Ignore ign;        // what isn't synced
        int lock_fd;       // held while syncing, so a second isf can't join
        BT seen;           // rel -> what the agent saw of it (State *) since the
                           // last flush; written: a write not by isf came
        BT expect;         // rel -> what isf just made of it there (Expect *),
                           // until the agent reports it
        struct {           // the remote folder as the agent or sync_check
                int listed;        // listed it, for the next walk of all of it
                int status;        // (then it's taken): SFTP_OK or
                                   // SFTP_NO_SUCH_FILE,
                SftpDir dir;       // what's in it, sorted,
                int has_state;     // and, if known,
                State state;       // what it is; and rel -> SftpDir *, the
                BT below;          // folders below as the agent listed them;
                int broken;        // a listing was damaged: none is used
        } seed;
} Root;

/* Call first. SFTP is the main connection (decisions and metadata), to HOST
 * and PORT (for messages and to tell records apart). JOBS is how many
 * parallel transfer connections to open (< 2: none, transfers run on SFTP);
 * SSH_OPTS go to those, like to the main one. DRY_RUN: plan and report, but
 * change nothing (and record nothing). QUIET: don't say each file sent or
 * received (conflicts still are, and sync_stats counts them). */
void sync_init(Sftp *sftp, const char *host, const char *port, char *const *ssh_opts, int jobs, int dry_run, int quiet);

/* Did the connection break? Then the steps that needed it failed, and
 * everything is synced again once connected (sync_init again). */
int sync_lost(void);

/* Wait for the parallel transfers of the current batch, record them, and set
 * the directory modes held back. Call after a run of reconcile(), before
 * saving. Returns 1 if a transfer connection broke (fatal). */
int sync_drain(void);
/* Stop the transfer pool (at exit). */
void sync_shutdown(void);

/* A run of reconcile() starts (ON), or ended, drained (0). If it goes on for
 * over 2 s, a status line on the terminal says how far it got: the remote
 * folders listed, then the files and bytes transferred of those planned so
 * far. */
void sync_progress(int on);

/* Take ROOT's lock, and load its record (or forget it first if RESET: then
 * the next sync is like the first one) and what it ignores. Returns 1
 * (logged) if it can't. */
int sync_open(Root *root, int reset);
/* The agent listed REL, a folder of ROOT, when it started: SELF is what it is,
 * and DIR (taken) what's in it, or some of it (the rest comes next). The next
 * walk of all of ROOT goes on from these listings, instead of listing the
 * folders over SFTP. A NULL DIR is a damaged listing: then none of ROOT's is
 * used, and its folders are listed over SFTP. */
void sync_remote_listed(Root *root, const char *rel, const SftpAttrs *self, SftpDir *dir);
/* Is syncing ROOT safe? Returns 1 (logged) if not: it was synced before, but
 * one side is empty now. Unless the agent listed the remote folder, it lists
 * it, and the next walk of all of it goes on from that listing: call it once
 * the agent watches, so nothing changed after it goes unreported. */
int sync_check(Root *root);
/* Forget those listings: they're from another agent, or isf itself changed
 * what's there since */
void sync_forget_seed(Root *root);
void record_save(Root *root);

/* Make both sides agree on REL, inside ROOT. WHAT is the SYNC_* of the change.
 * REMOTE_NOW is the remote state if known (from a listing), or NULL. Returns 1
 * if everything inside REL was handled too. */
int reconcile(Root *root, const char *rel, int what, const State *remote_now);

/* The agent saw REL, in ROOT, as SEEN, after an event of KIND (see agent.h).
 * The next reconcile goes by it, instead of asking the remote again. */
void sync_remote_seen(Root *root, const char *rel, const State *seen, char kind);
/* Forget them: after a flush */
void sync_forget_seen(Root *root);

/* Is REL, in ROOT, still what was last synced here (or gone, and not synced)?
 * Then an event about it is isf's own doing (a file it received or removed),
 * or changes nothing. */
int sync_unchanged(Root *root, const char *rel);

/* Call FN(ROOT, PATH) when a local directory is made, before anything goes in
 * it: to watch it right away */
void sync_on_local_dir(void (*fn)(Root *root, const char *path));

/* What was synced so far */
typedef struct {
        int sent, received, conflicts;
} SyncStats;
SyncStats sync_stats(void);

/* FROM was renamed to TO on the local side: rename the remote copy too, if
 * both are still what was last synced. Returns 1 if it did. */
int sync_rename(Root *root, const char *from, const char *to);

/* FROM was renamed to TO on the remote, which is SEEN there now: rename the
 * local copy too, if it's still what was last synced and TO has nothing here.
 * Returns 1 if it did. */
int sync_remote_rename(Root *root, const char *from, const char *to, const State *seen);

#endif // !SYNC_H_
