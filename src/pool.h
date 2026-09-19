#ifndef POOL_H_
#define POOL_H_

#include "sftp.h"

/* Parallel transfers. Each worker thread has its own SFTP connection; jobs
 * run on them and come back to the main thread when done (pool_drain), in no
 * particular order. The connections share nothing, so jobs must be
 * independent: the order-sensitive work stays on the main thread. */

/* Open JOBS connections to HOST (PORT may be NULL; SSH_OPTS, NULL terminated,
 * go to ssh). With JOBS < 2, or if they can't all be opened (logged), there is
 * no pool and pool_enabled() is 0. */
void pool_start(int jobs, const char *host, const char *port, char *const *ssh_opts);
int pool_enabled(void);

/* Jobs a worker takes at once, at most */
#define POOL_BATCH 64

/* Run RUN(CONN, ARGS, N) on a worker, with ARG among ARGS: a worker takes the
 * jobs waiting with the same RUN, up to POOL_BATCH, and runs them together. */
void pool_submit(void (*run)(Sftp *conn, void **args, int n), void *arg);

/* Wait for every job submitted, calling DONE(ARG) for each as it finishes, on
 * this thread, and TICK (if not NULL) about every 200 ms while waiting.
 * Returns 1 if a worker's connection broke: the pool can't be used anymore. */
int pool_drain(void (*done)(void *arg), void (*tick)(void));

/* Stop the workers and close their connections */
void pool_stop(void);

#endif // !POOL_H_
