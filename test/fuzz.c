/* Fuzz what comes from the other side: the agent's messages, the SFTP
 * server's replies and the .isfignore it sends, which a broken or hostile
 * remote could make up.
 *
 *     make fuzz CC=clang && test/fuzz -max_total_time=60
 *
 * The first byte of each input picks the target, the rest is the stream. */
#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "agent.h"
#include "ignore.h"
#include "sftp.h"
#include "sync.h"
#include "util.h"

/* A pipe holding DATA, read like what the other side sent. Returns its read
 * end, or -1. */
static int
pipe_of(const uint8_t *data, size_t size)
{
        int p[2];
        if (pipe(p) == -1) return -1;
        if (fcntl(p[1], F_SETFL, O_NONBLOCK) == -1 || write(p[1], data, size) == -1) { /* what fits */ }
        close(p[1]);
        return p[0];
}

static void
changed(char type, int root, const char *rel, const State *seen)
{
        (void) type, (void) root, (void) rel, (void) seen;
}

static void
moved(int root, const char *from, const char *to, const State *seen)
{
        (void) root, (void) from, (void) to, (void) seen;
}

static void
listed(int root, const char *rel, const SftpAttrs *self, SftpDir *dir)
{
        (void) root, (void) rel, (void) self;
        if (dir) sftp_dir_free(dir);
}

static void
placed(uint64_t id, char how)
{
        (void) id, (void) how;
}

static void
fuzz_agent(const uint8_t *data, size_t size)
{
        int fd = pipe_of(data, size);
        if (fd == -1) return;
        Agent a = { .pid = -1, .to = -1, .from = fd };
        while (!agent_read(&a, changed, moved, listed, placed))
                ;
        close(fd);
        Da_destroy(&a.buf);
        free(a.version);
}

static void
fuzz_sftp(const uint8_t *data, size_t size)
{
        int fd = pipe_of(data, size);
        if (fd == -1) return;
        int sink = open("/dev/null", O_RDWR | O_CLOEXEC);
        Sftp s   = { .pid = -1, .to = sink, .from = fd };

        SftpDir dir = { 0 };
        SftpAttrs attrs;
        char target[256];
        sftp_readdir(&s, "/x", &dir);
        sftp_dir_free(&dir);
        sftp_lstat(&s, "/x", &attrs);
        sftp_readlink(&s, "/x", target, sizeof target);
        SftpFile f = { .fd = sink, .path = "/x", .size = 4096 };
        sftp_get_many(&s, &f, 1);
        SftpOp op = { .op = SFTP_OP_LSTAT, .path = "/x" };
        sftp_batch(&s, &op, 1);

        Da_destroy(&s.buf);
        close(fd);
        close(sink);
}

/* The .isfignore of a folder comes from the other side like everything else:
 * whatever is in it, loading it and asking about paths must hold up. */
static void
fuzz_ignore(const uint8_t *data, size_t size)
{
        char dir[] = "/tmp/isf-fuzz.XXXXXX";
        if (mkdtemp(dir) == NULL) return;
        char path[64];
        snprintf(path, sizeof path, "%s/%s", dir, IGNORE_FILE);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd != -1) {
                ssize_t n = write(fd, data, size);
                (void) n;
                close(fd);
        }

        Ignore ign = { 0 };
        ignore_load(&ign, dir);
        static const char *const paths[] = { "", "a", "a/b", "a/b/c.log", ".hidden", "d/", "x/y/z/deep" };
        for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
                ignored(&ign, paths[i], 0);
                ignored(&ign, paths[i], 1);
        }
        Da_foreach(pat, ign)
        {
                free(pat->pattern);
        }
        Da_destroy(&ign);
        unlink(path);
        rmdir(dir);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
        static int quiet;
        if (!quiet) { // the errors it logs aren't what's interesting
                freopen("/dev/null", "w", stderr);
                quiet = 1;
        }
        if (size < 1) return 0;
        switch (data[0] % 3) {
        case 0: fuzz_agent(data + 1, size - 1); break;
        case 1: fuzz_sftp(data + 1, size - 1); break;
        default: fuzz_ignore(data + 1, size - 1); break;
        }
        return 0;
}
