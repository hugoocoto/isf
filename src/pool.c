#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "pool.h"
#include "util.h"

typedef struct {
        void (*run)(Sftp *conn, int worker, void *arg);
        void *arg;
} Job;

typedef Da(Job) Jobs;

static struct {
        int jobs;          // workers; < 2 means no pool
        Sftp *conn;        // [jobs]
        pthread_t *thread; // [jobs]
        pthread_mutex_t mtx;
        pthread_cond_t wake;  // a job is waiting, or stop
        pthread_cond_t idle;  // pending reached 0
        pthread_cond_t ready; // a worker finished connecting
        Jobs queue;           // waiting
        Jobs done;            // finished, for pool_drain
        int pending;          // submitted, not finished yet
        int stop;
        const char *host, *port;
        char *const *ssh_opts;
        int connected, failed; // workers that finished connecting
} pool;

int
pool_enabled(void)
{
        return pool.jobs >= 2;
}

static void *
worker_main(void *arg)
{
        int id  = (int) (intptr_t) arg;
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
                Job j = pool.queue.items[--pool.queue.count];
                pthread_mutex_unlock(&pool.mtx);

                j.run(c, id + 1, j.arg); // no lock held: the connections are separate

                pthread_mutex_lock(&pool.mtx);
                Da_append(&pool.done, j);
                if (--pool.pending == 0) pthread_cond_signal(&pool.idle);
                pthread_mutex_unlock(&pool.mtx);
        }
}

/* Stop and join the first N workers */
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
        pthread_cond_init(&pool.idle, NULL);
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
        pthread_mutex_lock(&pool.mtx);
        while (pool.connected + pool.failed < n)
                pthread_cond_wait(&pool.ready, &pool.mtx);
        pthread_mutex_unlock(&pool.mtx);

        if (n < jobs || pool.failed) {
                /* Couldn't open all of them: run synchronous rather than degraded */
                LOG_WARN("Using a single connection instead of %d", jobs);
                stop_workers(n);
                free(pool.conn);
                free(pool.thread);
                pool.conn = NULL;
                pool.jobs = 1; // disabled
        }
}

void
pool_submit(void (*run)(Sftp *conn, int worker, void *arg), void *arg)
{
        pthread_mutex_lock(&pool.mtx);
        Da_append(&pool.queue, (Job) { run, arg });
        pool.pending++;
        pthread_cond_signal(&pool.wake);
        pthread_mutex_unlock(&pool.mtx);
}

int
pool_drain(void (*done)(void *arg))
{
        if (!pool_enabled()) return 0;

        pthread_mutex_lock(&pool.mtx);
        while (pool.pending > 0)
                pthread_cond_wait(&pool.idle, &pool.mtx);
        Jobs finished = pool.done;
        pool.done     = (Jobs) { 0 };
        pthread_mutex_unlock(&pool.mtx);

        Da_foreach(j, finished)
        {
                done(j->arg);
        }
        Da_destroy(&finished);

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
