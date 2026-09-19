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

/* Run RUN(CONN, WORKER, ARG) on a worker. WORKER is its number, from 1. */
void pool_submit(void (*run)(Sftp *conn, int worker, void *arg), void *arg);

/* Wait for every job submitted, then call DONE(ARG) for each, on this thread.
 * Returns 1 if a worker's connection broke: the pool can't be used anymore. */
int pool_drain(void (*done)(void *arg));

/* Stop the workers and close their connections */
void pool_stop(void);

#endif // !POOL_H_
