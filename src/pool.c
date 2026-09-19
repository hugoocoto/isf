#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pool.h"
#include "util.h"

typedef struct {
        void (*run)(Sftp *conn, void **args, int n);
        void *arg;
} Job;

typedef Da(Job) Jobs;

static struct {
        int jobs;          // workers; < 2 means no pool
        Sftp *conn;        // [jobs]
        pthread_t *thread; // [jobs]
        pthread_mutex_t mtx;
        pthread_cond_t wake;     // a job is waiting, or stop
        pthread_cond_t finished; // a batch of jobs finished
        pthread_cond_t ready;    // a worker finished connecting
        Jobs queue;              // waiting
        Jobs done;               // finished, for pool_drain
        int pending;             // submitted, not finished yet
        int stop;
        const char *host, *port;
        char *const *ssh_opts;
        int connected, failed; // workers that finished connecting
        int starting;          // workers started, of JOBS: not waited for yet
} pool;

static void stop_workers(int n);

/* The workers connect in the background: the first time the pool is needed,
 * wait until they're done. If some couldn't, run synchronous rather than
 * degraded. */
static void
pool_settle(void)
{
        if (!pool.starting) return;
        int n = pool.starting > 0 ? pool.starting : 0;
        pthread_mutex_lock(&pool.mtx);
        while (pool.connected + pool.failed < n)
                pthread_cond_wait(&pool.ready, &pool.mtx);
        pthread_mutex_unlock(&pool.mtx);
        pool.starting = 0;
        if (n < pool.jobs || pool.failed) {
                LOG_WARN("Using a single connection instead of %d", pool.jobs);
                stop_workers(n); // and disabled
        }
}

int
pool_enabled(void)
{
        pool_settle();
        return pool.jobs >= 2;
}

static void *
worker_main(void *arg)
{
        int id  = (int) (intptr_t) arg; // its connection
        Sftp *c = &pool.conn[id];

        /* Each worker opens its connection, all at once: a few round trips
         * each, which would add up one after another */
        int bad = sftp_connect(c, pool.host, pool.port, pool.ssh_opts);
        if (bad) LOG("Error", "Cannot open transfer connection: %s", c->error);
        pthread_mutex_lock(&pool.mtx);
        if (bad)
                pool.failed++;
        else
                pool.connected++;
        pthread_cond_signal(&pool.ready);
        pthread_mutex_unlock(&pool.mtx);
        if (bad) return NULL;

        for (;;) {
                pthread_mutex_lock(&pool.mtx);
                while (pool.queue.count == 0 && !pool.stop)
                        pthread_cond_wait(&pool.wake, &pool.mtx);
                if (pool.queue.count == 0) { // stop, and nothing left
                        pthread_mutex_unlock(&pool.mtx);
                        return NULL;
                }
                /* The newest job, and the ones before it with the same run */
                Job batch[POOL_BATCH];
                void *args[POOL_BATCH];
                int n = 0;
                do {
                        batch[n] = pool.queue.items[--pool.queue.count];
                        args[n]  = batch[n].arg;
                        n++;
                } while (n < POOL_BATCH && pool.queue.count > 0 &&
                         pool.queue.items[pool.queue.count - 1].run == batch[0].run);
                pthread_mutex_unlock(&pool.mtx);

                batch[0].run(c, args, n); // no lock held: the connections are separate

                pthread_mutex_lock(&pool.mtx);
                for (int i = 0; i < n; i++)
                        Da_append(&pool.done, batch[i]);
                pool.pending -= n;
                pthread_cond_signal(&pool.finished);
                pthread_mutex_unlock(&pool.mtx);
        }
}

/* Stop and join the first N workers, close their connections, and forget
 * it all: pool_start can start it again */
static void
stop_workers(int n)
{
        pthread_mutex_lock(&pool.mtx);
        pool.stop = 1;
        pthread_cond_broadcast(&pool.wake);
        pthread_mutex_unlock(&pool.mtx);
        for (int i = 0; i < n; i++) {
                pthread_join(pool.thread[i], NULL);
                sftp_disconnect(&pool.conn[i]);
        }
        free(pool.conn);
        free(pool.thread);
        Da_destroy(&pool.queue);
        Da_destroy(&pool.done);
        pthread_mutex_destroy(&pool.mtx);
        pthread_cond_destroy(&pool.wake);
        pthread_cond_destroy(&pool.finished);
        pthread_cond_destroy(&pool.ready);
        memset(&pool, 0, sizeof pool);
}

void
pool_start(int jobs, const char *host, const char *port, char *const *ssh_opts)
{
        pool.jobs = jobs;
        if (!pool_enabled()) return;
        pool.conn   = calloc(jobs, sizeof *pool.conn);
        pool.thread = calloc(jobs, sizeof *pool.thread);
        assert(pool.conn && pool.thread);
        pthread_mutex_init(&pool.mtx, NULL);
        pthread_cond_init(&pool.wake, NULL);
        pthread_cond_init(&pool.finished, NULL);
        pthread_cond_init(&pool.ready, NULL);
        pool.host     = host;
        pool.port     = port;
        pool.ssh_opts = ssh_opts;

        int n = 0;
        for (; n < jobs; n++) {
                if (pthread_create(&pool.thread[n], NULL, worker_main, (void *) (intptr_t) n)) {
                        LOG_ERR("Cannot start a transfer thread");
                        break;
                }
        }
        /* They connect while the rest starts: see pool_settle */
        pool.starting = n ? n : -1;
        if (n == 0) pool_settle();
}

void
pool_submit(void (*run)(Sftp *conn, void **args, int n), void *arg)
{
        pthread_mutex_lock(&pool.mtx);
        Da_append(&pool.queue, (Job) { run, arg });
        pool.pending++;
        pthread_cond_signal(&pool.wake);
        pthread_mutex_unlock(&pool.mtx);
}

int
pool_drain(void (*done)(void *arg), void (*tick)(void))
{
        if (!pool_enabled()) return 0;

        pthread_mutex_lock(&pool.mtx);
        for (;;) {
                Jobs finished = pool.done;
                pool.done     = (Jobs) { 0 };
                int left      = pool.pending;
                pthread_mutex_unlock(&pool.mtx);

                Da_foreach(j, finished)
                {
                        done(j->arg);
                }
                Da_destroy(&finished);
                if (left == 0) break;
                if (tick) tick();

                pthread_mutex_lock(&pool.mtx);
                struct timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += 200 * 1000000; // then TICK again
                if (until.tv_nsec >= 1000000000) {
                        until.tv_sec++;
                        until.tv_nsec -= 1000000000;
                }
                while (pool.done.count == 0 && pool.pending > 0)
                        if (pthread_cond_timedwait(&pool.finished, &pool.mtx, &until) == ETIMEDOUT) break;
        }

        for (int i = 0; i < pool.jobs; i++)
                if (pool.conn[i].dead) return 1;
        return 0;
}

void
pool_stop(void)
{
        if (!pool_enabled()) return;
        stop_workers(pool.jobs);
}
