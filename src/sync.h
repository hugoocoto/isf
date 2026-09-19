#ifndef SYNC_H_
#define SYNC_H_

#include <stdint.h>

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
} Root;

/* Call first. SFTP is the main connection (decisions and metadata), to HOST
 * and PORT (for messages and to tell records apart). JOBS is how many
 * parallel transfer connections to open (< 2: none, transfers run on SFTP);
 * SSH_OPTS go to those, like to the main one. DRY_RUN: plan and report, but
 * change nothing (and record nothing). */
void sync_init(Sftp *sftp, const char *host, const char *port, char *const *ssh_opts, int jobs, int dry_run);

/* Wait for the parallel transfers of the current batch, record them, and set
 * the directory modes held back. Call after a run of reconcile(), before
 * saving. Returns 1 if a transfer connection broke (fatal). */
int sync_drain(void);
/* Stop the transfer pool (at exit). */
void sync_shutdown(void);

/* Load ROOT's record (or forget it first if RESET: then the next sync is like
 * the first one) and what it ignores. Returns 1 (logged) if syncing looks unsafe: synced before,
 * but one side is empty now. */
int sync_open(Root *root, int reset);
void record_save(Root *root);

/* Make both sides agree on REL, inside ROOT. WHAT is the SYNC_* of the change.
 * REMOTE_NOW is the remote state if known (from a listing), or NULL. Returns 1
 * if everything inside REL was handled too. */
int reconcile(Root *root, const char *rel, int what, const State *remote_now);

/* What was synced so far */
typedef struct {
        int sent, received, conflicts;
} SyncStats;
SyncStats sync_stats(void);

/* FROM was renamed to TO on the local side: rename the remote copy too, if
 * both are still what was last synced. Returns 1 if it did. */
int sync_rename(Root *root, const char *from, const char *to);

#endif // !SYNC_H_
