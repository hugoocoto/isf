#define _GNU_SOURCE // pipe2

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sftp.h"

#define SFTP_VERSION 3

/* Largest packet we accept, same limit as OpenSSH's sftp-server */
#define SFTP_MAX_PACKET (256 * 1024)

/* Packet types */
enum {
        FXP_INIT           = 1,
        FXP_VERSION        = 2,
        FXP_OPEN           = 3,
        FXP_CLOSE          = 4,
        FXP_READ           = 5,
        FXP_WRITE          = 6,
        FXP_LSTAT          = 7,
        FXP_FSTAT          = 8,
        FXP_SETSTAT        = 9,
        FXP_FSETSTAT       = 10,
        FXP_OPENDIR        = 11,
        FXP_READDIR        = 12,
        FXP_REMOVE         = 13,
        FXP_MKDIR          = 14,
        FXP_RMDIR          = 15,
        FXP_REALPATH       = 16,
        FXP_STAT           = 17,
        FXP_RENAME         = 18,
        FXP_READLINK       = 19,
        FXP_SYMLINK        = 20,
        FXP_STATUS         = 101,
        FXP_HANDLE         = 102,
        FXP_DATA           = 103,
        FXP_NAME           = 104,
        FXP_ATTRS          = 105,
        FXP_EXTENDED       = 200,
        FXP_EXTENDED_REPLY = 201,
};

/* OPEN flags */
enum {
        FXF_READ   = 0x01,
        FXF_WRITE  = 0x02,
        FXF_APPEND = 0x04,
        FXF_CREAT  = 0x08,
        FXF_TRUNC  = 0x10,
        FXF_EXCL   = 0x20,
};

/* sftp_put sends the file in WRITEs of SFTP_WRITE_LEN bytes (the size every
 * server has to accept), with up to SFTP_MAX_INFLIGHT of them sent before
 * waiting for replies, so the speed doesn't depend on the round trip time. */
#define SFTP_WRITE_LEN (32 * 1024)
#define SFTP_MAX_INFLIGHT 16

/* Reads a received packet. Reading past the end sets BAD and returns zeros,
 * so a whole reply can be parsed and checked once at the end. */
typedef struct {
        const unsigned char *p;
        size_t left;
        int bad;
} Reader;

/* An open remote file or directory */
typedef struct {
        char data[256]; // the spec caps handles at 256 bytes
        uint32_t len;
} Handle;

__attribute__((format(printf, 3, 4))) static int
fail(Sftp *s, int status, const char *fmt, ...)
{
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s->error, sizeof s->error, fmt, ap);
        va_end(ap);
        if (status == SFTP_ERR_IO) s->dead = 1;
        return status;
}

static int
write_all(int fd, const void *data, size_t len)
{
        const char *p = data;
        while (len > 0) {
                ssize_t n = write(fd, p, len);
                if (n == -1) {
                        if (errno == EINTR) continue;
                        return -1;
                }
                p += n;
                len -= n;
        }
        return 0;
}

/* On EOF returns -1 with errno set to 0 */
static int
read_all(int fd, void *data, size_t len)
{
        char *p = data;
        while (len > 0) {
                ssize_t n = read(fd, p, len);
                if (n == 0) {
                        errno = 0;
                        return -1;
                }
                if (n == -1) {
                        if (errno == EINTR) continue;
                        return -1;
                }
                p += n;
                len -= n;
        }
        return 0;
}

/* Make room for LEN more bytes in s->buf */
static void
reserve(Sftp *s, size_t len)
{
        size_t need = s->buf.count + len;
        if (need <= (size_t) s->buf.capacity) return;
        size_t cap = s->buf.capacity ? s->buf.capacity : 1024;
        while (cap < need)
                cap *= 2;
        s->buf.items    = realloc(s->buf.items, cap);
        s->buf.capacity = cap;
        assert(s->buf.items);
}

static void
put(Sftp *s, const void *data, size_t len)
{
        reserve(s, len);
        memcpy(s->buf.items + s->buf.count, data, len);
        s->buf.count += len;
}

static void
put_u8(Sftp *s, uint8_t v)
{
        put(s, &v, 1);
}

static void
put_u32(Sftp *s, uint32_t v)
{
        unsigned char b[4] = { v >> 24, v >> 16, v >> 8, v };
        put(s, b, 4);
}

static void
put_u64(Sftp *s, uint64_t v)
{
        put_u32(s, v >> 32);
        put_u32(s, v);
}

static void
put_str(Sftp *s, const char *str)
{
        size_t len = strlen(str);
        put_u32(s, len);
        put(s, str, len);
}

static void
put_attrs(Sftp *s, const SftpAttrs *a)
{
        put_u32(s, a->flags & ~SFTP_ATTR_EXTENDED); // we never send extensions
        if (a->flags & SFTP_ATTR_SIZE) put_u64(s, a->size);
        if (a->flags & SFTP_ATTR_UIDGID) {
                put_u32(s, a->uid);
                put_u32(s, a->gid);
        }
        if (a->flags & SFTP_ATTR_PERMISSIONS) put_u32(s, a->perm);
        if (a->flags & SFTP_ATTR_ACMODTIME) {
                put_u32(s, a->atime);
                put_u32(s, a->mtime);
        }
}

static const unsigned char *
get(Reader *r, size_t len)
{
        if (r->bad || r->left < len) {
                r->bad  = 1;
                r->left = 0;
                return NULL;
        }
        const unsigned char *p = r->p;
        r->p += len;
        r->left -= len;
        return p;
}

static uint8_t
get_u8(Reader *r)
{
        const unsigned char *p = get(r, 1);
        return p ? p[0] : 0;
}

static uint32_t
get_u32(Reader *r)
{
        const unsigned char *p = get(r, 4);
        return p ? (uint32_t) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3] : 0;
}

static uint64_t
get_u64(Reader *r)
{
        uint64_t hi = get_u32(r);
        return hi << 32 | get_u32(r);
}

/* The string is not NUL terminated, its length goes to LEN */
static const char *
get_str(Reader *r, uint32_t *len)
{
        *len = get_u32(r);
        return (const char *) get(r, *len);
}

static void
get_attrs(Reader *r, SftpAttrs *a)
{
        *a = (SftpAttrs) { .flags = get_u32(r) };
        if (a->flags & SFTP_ATTR_SIZE) a->size = get_u64(r);
        if (a->flags & SFTP_ATTR_UIDGID) {
                a->uid = get_u32(r);
                a->gid = get_u32(r);
        }
        if (a->flags & SFTP_ATTR_PERMISSIONS) a->perm = get_u32(r);
        if (a->flags & SFTP_ATTR_ACMODTIME) {
                a->atime = get_u32(r);
                a->mtime = get_u32(r);
        }
        if (a->flags & SFTP_ATTR_EXTENDED) {
                uint32_t len;
                for (uint32_t n = get_u32(r); n > 0 && !r->bad; n--) {
                        get_str(r, &len); // name
                        get_str(r, &len); // data
                }
        }
}

/* Start a packet in s->buf: the length (filled by packet_send) and the type */
static void
packet_begin(Sftp *s, uint8_t type)
{
        s->buf.count = 0;
        put_u32(s, 0);
        put_u8(s, type);
}

static int
packet_send(Sftp *s)
{
        uint32_t len     = s->buf.count - 4;
        unsigned char *p = s->buf.items;
        p[0]             = len >> 24;
        p[1]             = len >> 16;
        p[2]             = len >> 8;
        p[3]             = len;
        if (write_all(s->to, s->buf.items, s->buf.count))
                return fail(s, SFTP_ERR_IO, "write to ssh: %s", strerror(errno));
        return SFTP_OK;
}

/* Read one whole packet into s->buf (its type and body, no request id read
 * yet). */
static int
recv_raw(Sftp *s)
{
        unsigned char hdr[4];
        if (read_all(s->from, hdr, sizeof hdr)) goto read_error;

        uint32_t len = (uint32_t) hdr[0] << 24 | hdr[1] << 16 | hdr[2] << 8 | hdr[3];
        if (len < 1 || len > SFTP_MAX_PACKET)
                return fail(s, SFTP_ERR_IO, "bad packet length %u", len);

        s->buf.count = 0;
        reserve(s, len);
        if (read_all(s->from, s->buf.items, len)) goto read_error;
        s->buf.count = len;
        return SFTP_OK;

read_error:
        return fail(s, SFTP_ERR_IO, "read from ssh: %s",
                    errno ? strerror(errno) : "connection closed");
}

static int
packet_recv(Sftp *s, uint8_t *type, Reader *r)
{
        /* Discard the replies of fire-and-forget requests (directory CLOSEs):
         * they sit at the front of the stream, before the reply we want. */
        while (s->pending > 0) {
                int st = recv_raw(s);
                if (st) return st;
                s->pending--;
        }
        int st = recv_raw(s);
        if (st) return st;

        *r    = (Reader) { .p = s->buf.items, .left = s->buf.count };
        *type = get_u8(r);
        return SFTP_OK;
}


/* Start a request packet: type and a new id. Returns the id. */
static uint32_t
request_begin(Sftp *s, uint8_t type)
{
        uint32_t id = s->next_id++;
        packet_begin(s, type);
        put_u32(s, id);
        return id;
}

/* Send the request in s->buf and read its reply. R reads what follows the
 * reply id. */
static int
request_send(Sftp *s, uint32_t id, uint8_t *type, Reader *r)
{
        int st;
        if ((st = packet_send(s)) || (st = packet_recv(s, type, r))) return st;
        uint32_t reply = get_u32(r);
        if (r->bad || reply != id)
                return fail(s, SFTP_ERR_IO, "reply %u does not match request %u", reply, id);
        return SFTP_OK;
}

static const char *
status_name(uint32_t code)
{
        static const char *names[] = {
                "ok",
                "end of file",
                "no such file",
                "permission denied",
                "failure",
                "bad message",
                "no connection",
                "connection lost",
                "operation unsupported",
        };
        return code < sizeof names / sizeof *names ? names[code] : "unknown error";
}

/* Parse a STATUS reply. Returns its code; the server message goes to
 * s->error. */
static int
read_status(Sftp *s, uint8_t type, Reader *r)
{
        if (type != FXP_STATUS) return fail(s, SFTP_ERR_IO, "unexpected reply type %u", type);

        uint32_t code = get_u32(r);
        if (r->bad) return fail(s, SFTP_ERR_IO, "truncated STATUS reply");
        if (code == SFTP_OK) return SFTP_OK;
        if (code > INT_MAX) code = SFTP_FAILURE;

        /* The message is optional: fall back to the code name */
        uint32_t len;
        const char *msg = get_str(r, &len);
        if (msg && len)
                fail(s, 0, "%.*s", (int) len, msg);
        else
                fail(s, 0, "%s", status_name(code));
        return code;
}

/* Send the request in s->buf and wait for a STATUS reply */
static int
request_status(Sftp *s, uint32_t id)
{
        uint8_t type;
        Reader r;
        int st = request_send(s, id, &type, &r);
        return st ? st : read_status(s, type, &r);
}

static int
handshake(Sftp *s)
{
        /* INIT has no request id, only the version we speak */
        packet_begin(s, FXP_INIT);
        put_u32(s, SFTP_VERSION);

        uint8_t type;
        Reader r;
        int st;
        if ((st = packet_send(s)) || (st = packet_recv(s, &type, &r))) return st;
        if (type != FXP_VERSION)
                return fail(s, SFTP_ERR_IO, "expected VERSION reply, got type %u", type);

        uint32_t version = get_u32(&r);
        if (r.bad || version < SFTP_VERSION)
                return fail(s, SFTP_ERR_IO, "server speaks SFTP %u, need %d", version, SFTP_VERSION);

        /* The rest are (name, data) pairs of extensions the server supports */
        while (r.left > 0) {
                uint32_t len, data_len;
                const char *name = get_str(&r, &len);
                get_str(&r, &data_len);
                if (r.bad) return fail(s, SFTP_ERR_IO, "truncated VERSION reply");

                const char *ext = "posix-rename@openssh.com";
                if (len == strlen(ext) && !memcmp(name, ext, len)) s->posix_rename = 1;
        }
        return SFTP_OK;
}

pid_t
spawn_piped(char *const argv[], int *to, int *from)
{
        /* O_CLOEXEC so other children don't inherit them */
        int in[2], out[2];
        if (pipe2(in, O_CLOEXEC) == -1) return -1;
        if (pipe2(out, O_CLOEXEC) == -1) {
                close(in[0]);
                close(in[1]);
                return -1;
        }

        pid_t pid = fork();
        if (pid == 0) {
                /* dup2 clears O_CLOEXEC on 0 and 1, the pipe ends get closed on exec */
                dup2(in[0], STDIN_FILENO);
                dup2(out[1], STDOUT_FILENO);
                execvp(argv[0], argv);
                fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
                _exit(127);
        }
        int fork_errno = errno;
        close(in[0]);
        close(out[1]);
        if (pid == -1) {
                close(in[1]);
                close(out[0]);
                errno = fork_errno;
                return -1;
        }

        /* If the child dies, writing to it has to fail with EPIPE instead of
         * killing us. Set after fork so the child keeps the default. */
        signal(SIGPIPE, SIG_IGN);
        *to   = in[1];
        *from = out[0];
        return pid;
}

int
sftp_connect(Sftp *s, const char *host, const char *port, char *const *opts)
{
        *s = (Sftp) { .pid = -1, .to = -1, .from = -1 };

        /* Same options sftp(1) passes, so forwardings or a RemoteCommand in
         * ~/.ssh/config don't get in the way of the subsystem */
        Command c = { 0 };
        Command_add(&c, "ssh", "-x", "-a", "-oClearAllForwardings=yes",
                    "-oPermitLocalCommand=no", "-oRemoteCommand=none", "-oRequestTTY=no");
        for (; opts && *opts; opts++)
                Command_add(&c, *opts);
        if (port) Command_add(&c, "-p", port);
        Command_add(&c, "-s", "--", host, "sftp", NULL);

        s->pid = spawn_piped(c.items, &s->to, &s->from);
        Command_destroy(&c);
        if (s->pid == -1) return fail(s, SFTP_ERR_IO, "cannot run ssh: %s", strerror(errno));

        int st = handshake(s);
        if (st) sftp_disconnect(s);
        return st;
}

void
sftp_disconnect(Sftp *s)
{
        /* EOF on its stdin ends sftp-server, and then ssh */
        if (s->to != -1) close(s->to);
        if (s->from != -1) close(s->from);
        if (s->pid > 0) waitpid(s->pid, NULL, 0);
        Da_destroy(&s->buf);
        s->to = s->from = s->pid = -1;
}

/* A reply of the wrong type has to be an error STATUS */
static int
not_expected(Sftp *s, uint8_t type, Reader *r)
{
        int st = read_status(s, type, r);
        return st ? st : fail(s, SFTP_ERR_IO, "got an OK status instead of data");
}

/* Send the request in s->buf and read the HANDLE reply */
static int
request_handle(Sftp *s, uint32_t id, Handle *h)
{
        uint8_t type;
        Reader r;
        int st = request_send(s, id, &type, &r);
        if (st) return st;
        if (type != FXP_HANDLE) return not_expected(s, type, &r);

        const char *data = get_str(&r, &h->len);
        if (r.bad || h->len > sizeof h->data) return fail(s, SFTP_ERR_IO, "bad HANDLE reply");
        memcpy(h->data, data, h->len);
        return SFTP_OK;
}

static void
put_handle(Sftp *s, const Handle *h)
{
        put_u32(s, h->len);
        put(s, h->data, h->len);
}

static int
close_handle(Sftp *s, const Handle *h)
{
        uint32_t id = request_begin(s, FXP_CLOSE);
        put_handle(s, h);
        return request_status(s, id);
}

/* Close a directory handle without waiting for the reply — it can't fail in a
 * way that matters (we already read everything), and skipping the round trip
 * makes the tree walk quicker. The reply is drained before the next read. */
static int
close_handle_async(Sftp *s, const Handle *h)
{
        request_begin(s, FXP_CLOSE);
        put_handle(s, h);
        int st = packet_send(s);
        if (st == SFTP_OK) s->pending++;
        return st;
}

/* STAT or LSTAT */
static int
stat_request(Sftp *s, uint8_t type, const char *path, SftpAttrs *attrs)
{
        uint32_t id = request_begin(s, type);
        put_str(s, path);

        Reader r;
        int st = request_send(s, id, &type, &r);
        if (st) return st;
        if (type != FXP_ATTRS) return not_expected(s, type, &r);
        get_attrs(&r, attrs);
        if (r.bad) return fail(s, SFTP_ERR_IO, "truncated ATTRS reply");
        return SFTP_OK;
}

/* Requests that take a path and reply with a STATUS */
static int
path_request(Sftp *s, uint8_t type, const char *path)
{
        uint32_t id = request_begin(s, type);
        put_str(s, path);
        return request_status(s, id);
}

int
sftp_stat(Sftp *s, const char *path, SftpAttrs *attrs)
{
        return stat_request(s, FXP_STAT, path, attrs);
}

int
sftp_lstat(Sftp *s, const char *path, SftpAttrs *attrs)
{
        return stat_request(s, FXP_LSTAT, path, attrs);
}

int
sftp_setstat(Sftp *s, const char *path, const SftpAttrs *attrs)
{
        uint32_t id = request_begin(s, FXP_SETSTAT);
        put_str(s, path);
        put_attrs(s, attrs);
        return request_status(s, id);
}

int
sftp_remove(Sftp *s, const char *path)
{
        return path_request(s, FXP_REMOVE, path);
}

int
sftp_rmdir(Sftp *s, const char *path)
{
        return path_request(s, FXP_RMDIR, path);
}

int
sftp_mkdir(Sftp *s, const char *path)
{
        uint32_t id = request_begin(s, FXP_MKDIR);
        put_str(s, path);
        put_attrs(s, &(SftpAttrs) { 0 }); // server default mode: 0777 & ~umask
        return request_status(s, id);
}

static int
is_dir(const SftpAttrs *a)
{
        return (a->flags & SFTP_ATTR_PERMISSIONS) && S_ISDIR(a->perm);
}

int
sftp_mkdir_p(Sftp *s, const char *path)
{
        if (*path == 0) return fail(s, SFTP_FAILURE, "empty path");

        /* Usually the parent already exists (dirs are created top-down), so
         * one MKDIR does it. Only when that fails do we look at the chain. */
        int st = sftp_mkdir(s, path);
        if (st == SFTP_OK || st == SFTP_ERR_IO) return st;

        SftpAttrs a;
        st = sftp_stat(s, path, &a);
        if (st == SFTP_OK) return is_dir(&a) ? SFTP_OK : fail(s, SFTP_FAILURE, "'%s' is not a directory", path);
        if (st != SFTP_NO_SUCH_FILE) return st;

        /* A parent is missing: go down from the top, creating what isn't there.
         * Searching from p + 1 skips the leading slash of absolute paths. */
        char *p     = strdup(path);
        char *slash = p;
        do {
                slash = strchr(slash + 1, '/');
                if (slash) *slash = 0;
                st = sftp_stat(s, p, &a);
                if (st == SFTP_NO_SUCH_FILE)
                        st = sftp_mkdir(s, p);
                else if (st == SFTP_OK && !is_dir(&a))
                        st = fail(s, SFTP_FAILURE, "'%s' is not a directory", p);
                if (slash) *slash = '/';
        } while (st == SFTP_OK && slash);
        free(p);
        return st;
}

static int
rename_request(Sftp *s, const char *from, const char *to)
{
        uint32_t id;
        if (s->posix_rename) {
                id = request_begin(s, FXP_EXTENDED);
                put_str(s, "posix-rename@openssh.com");
        } else {
                id = request_begin(s, FXP_RENAME);
        }
        put_str(s, from);
        put_str(s, to);
        return request_status(s, id);
}

int
sftp_rename(Sftp *s, const char *from, const char *to)
{
        int st = rename_request(s, from, to);
        if (st != SFTP_FAILURE || s->posix_rename) return st;

        /* A v3 RENAME fails if TO exists. Remove it and try again (not
         * atomic). If TO wasn't the problem, keep the first error. */
        char error[sizeof s->error];
        memcpy(error, s->error, sizeof error);
        int rm = sftp_remove(s, to);
        if (rm == SFTP_ERR_IO) return rm;
        if (rm == SFTP_OK) return rename_request(s, from, to);
        memcpy(s->error, error, sizeof error);
        return st;
}

int
sftp_symlink(Sftp *s, const char *target, const char *link)
{
        /* The spec puts LINK first, but OpenSSH's sftp-server was written with
         * the arguments swapped, and clients follow OpenSSH */
        uint32_t id = request_begin(s, FXP_SYMLINK);
        put_str(s, target);
        put_str(s, link);
        return request_status(s, id);
}

int
sftp_readlink(Sftp *s, const char *path, char *target, size_t size)
{
        uint32_t id = request_begin(s, FXP_READLINK);
        put_str(s, path);

        uint8_t type;
        Reader r;
        int st = request_send(s, id, &type, &r);
        if (st) return st;
        if (type != FXP_NAME) return not_expected(s, type, &r);

        /* A NAME reply with a single entry: the target */
        uint32_t count   = get_u32(&r), len;
        const char *name = get_str(&r, &len);
        if (r.bad || count != 1) return fail(s, SFTP_ERR_IO, "bad READLINK reply");
        if (len >= size) return fail(s, SFTP_FAILURE, "link target too long");
        memcpy(target, name, len);
        target[len] = 0;
        return SFTP_OK;
}

/* Send everything readable from FD in pipelined WRITEs. The server answers
 * requests in order, so the replies are for ids oldest, oldest + 1... After
 * an error nothing else is sent, but the replies in flight are still read to
 * keep the stream in sync. */
static int
write_from(Sftp *s, const Handle *h, int fd)
{
        /* Not static: parallel transfers run this on several threads at once */
        char chunk[SFTP_WRITE_LEN];
        uint64_t offset = 0;
        uint32_t oldest = s->next_id; // oldest WRITE without reply
        int eof = 0, st = SFTP_OK;

        while ((!eof && st == SFTP_OK) || oldest != s->next_id) {
                if (!eof && st == SFTP_OK && s->next_id - oldest < SFTP_MAX_INFLIGHT) {
                        ssize_t n = read(fd, chunk, sizeof chunk);
                        if (n == -1 && errno == EINTR) continue;
                        if (n == -1) {
                                st = fail(s, SFTP_FAILURE, "read: %s", strerror(errno));
                                continue;
                        }
                        if (n == 0) {
                                eof = 1;
                                continue;
                        }
                        request_begin(s, FXP_WRITE);
                        put_handle(s, h);
                        put_u64(s, offset);
                        put_u32(s, n);
                        put(s, chunk, n);
                        int io = packet_send(s);
                        if (io) return io;
                        offset += n;
                        continue;
                }

                uint8_t type;
                Reader r;
                int io = packet_recv(s, &type, &r);
                if (io) return io;
                uint32_t id = get_u32(&r);
                if (r.bad || id != oldest)
                        return fail(s, SFTP_ERR_IO, "reply %u does not match request %u", id, oldest);
                ++oldest;
                int ws = read_status(s, type, &r);
                if (ws == SFTP_ERR_IO) return ws;
                if (st == SFTP_OK) st = ws; // keep the first error
        }
        return st;
}

int
sftp_put(Sftp *s, int fd, const char *path, const SftpAttrs *attrs)
{
        uint32_t id = request_begin(s, FXP_OPEN);
        put_str(s, path);
        put_u32(s, FXF_WRITE | FXF_CREAT | FXF_TRUNC);
        put_attrs(s, &(SftpAttrs) { 0 });

        Handle h;
        int st = request_handle(s, id, &h);
        if (st) return st;

        st = write_from(s, &h, fd);
        if (st == SFTP_OK && attrs) {
                id = request_begin(s, FXP_FSETSTAT);
                put_handle(s, &h);
                put_attrs(s, attrs);
                st = request_status(s, id);
        }
        if (st == SFTP_ERR_IO) return st;

        int cst = close_handle(s, &h);
        return st ? st : cst;
}

static int
pwrite_all(int fd, const char *data, size_t len, uint64_t offset)
{
        while (len > 0) {
                ssize_t n = pwrite(fd, data, len, offset);
                if (n == -1) {
                        if (errno == EINTR) continue;
                        return -1;
                }
                data += n;
                len -= n;
                offset += n;
        }
        return 0;
}

/* Read the whole file behind H into FD with pipelined READs, like
 * write_from. Each request remembers its range in SLOT, so after a short
 * read the rest can be asked for again. Reading past the end gives EOF. */
static int
read_into(Sftp *s, const Handle *h, int fd)
{
        struct {
                uint64_t offset;
                uint32_t len;
        } slot[SFTP_MAX_INFLIGHT];
        uint64_t next   = 0;          // where the next new READ starts
        uint32_t oldest = s->next_id; // oldest READ without reply
        int eof = 0, st = SFTP_OK;
        uint64_t retry_offset = 0;
        uint32_t retry_len    = 0; // the rest of a short read, to ask again

        while ((!eof && st == SFTP_OK) || oldest != s->next_id) {
                if (!eof && st == SFTP_OK && s->next_id - oldest < SFTP_MAX_INFLIGHT) {
                        uint64_t offset = retry_len ? retry_offset : next;
                        uint32_t len    = retry_len ? retry_len : SFTP_WRITE_LEN;
                        if (retry_len)
                                retry_len = 0;
                        else
                                next += len;

                        uint32_t id = request_begin(s, FXP_READ);
                        put_handle(s, h);
                        put_u64(s, offset);
                        put_u32(s, len);
                        slot[id % SFTP_MAX_INFLIGHT].offset = offset;
                        slot[id % SFTP_MAX_INFLIGHT].len    = len;
                        int io                              = packet_send(s);
                        if (io) return io;
                        continue;
                }

                uint8_t type;
                Reader r;
                int io = packet_recv(s, &type, &r);
                if (io) return io;
                uint32_t id = get_u32(&r);
                if (r.bad || id != oldest)
                        return fail(s, SFTP_ERR_IO, "reply %u does not match request %u", id, oldest);
                ++oldest;

                if (type != FXP_DATA) {
                        int rs = read_status(s, type, &r);
                        if (rs == SFTP_ERR_IO) return rs;
                        if (rs == SFTP_EOF)
                                eof = 1;
                        else if (st == SFTP_OK)
                                st = rs ? rs : fail(s, SFTP_ERR_IO, "READ replied OK without data");
                        continue;
                }

                uint32_t len;
                const char *data = get_str(&r, &len);
                uint64_t offset  = slot[id % SFTP_MAX_INFLIGHT].offset;
                uint32_t want    = slot[id % SFTP_MAX_INFLIGHT].len;
                if (r.bad || len > want) return fail(s, SFTP_ERR_IO, "bad DATA reply");
                if (st == SFTP_OK && pwrite_all(fd, data, len, offset))
                        st = fail(s, SFTP_FAILURE, "write: %s", strerror(errno));
                if (len < want && !eof) {
                        /* Short read: usually the end of the file, and asking
                         * again gives EOF. The retry goes out next. */
                        retry_offset = offset + len;
                        retry_len    = want - len;
                }
        }
        return st;
}

int
sftp_get(Sftp *s, const char *path, int fd)
{
        uint32_t id = request_begin(s, FXP_OPEN);
        put_str(s, path);
        put_u32(s, FXF_READ);
        put_attrs(s, &(SftpAttrs) { 0 });

        Handle h;
        int st = request_handle(s, id, &h);
        if (st) return st;

        st = read_into(s, &h, fd);
        if (st == SFTP_ERR_IO) return st;
        int cst = close_handle(s, &h);
        return st ? st : cst;
}

/* Parse the entries of one FXP_NAME reply into DIR (without "." and ".."). */
static int
parse_names(Sftp *s, Reader *r, SftpDir *dir)
{
        for (uint32_t n = get_u32(r); n > 0 && !r->bad; n--) {
                uint32_t len, longname_len;
                const char *name = get_str(r, &len);
                get_str(r, &longname_len); // `ls -l` style line
                SftpAttrs attrs;
                get_attrs(r, &attrs);
                if (r->bad) break;
                if ((len == 1 && name[0] == '.') || (len == 2 && !memcmp(name, "..", 2)))
                        continue;
                Da_append(dir, (SftpEntry) { .name = strndup(name, len), .attrs = attrs });
        }
        return r->bad ? fail(s, SFTP_ERR_IO, "truncated NAME reply") : SFTP_OK;
}

/* sftp_readdir_many sends a round of requests before reading any reply, and
 * the server holds the replies until they are read: OpenSSH's sftp-server
 * queues them in memory, up to a few pages of entries per directory. So the
 * rounds are capped, to keep that small on a small remote: this many
 * directories, or this many bytes of paths. A wider level takes a few rounds. */
#define READDIR_MANY_DIRS 128
#define READDIR_MANY_BYTES (32 * 1024)

/* sftp_readdir_many for a batch small enough to pipeline */
static int
readdir_batch(Sftp *s, char *const *paths, int n, SftpDir *out, int *status)
{

        Handle *h  = malloc(n * sizeof *h);
        int *open  = calloc(n, sizeof *open); // 1 while its handle is usable
        int *eof   = calloc(n, sizeof *eof);
        int *order = malloc(n * sizeof *order); // reply order -> dir index
        assert(h && open && eof && order);
        int st = SFTP_OK;

        /* Open them all */
        uint32_t base = s->next_id;
        for (int i = 0; i < n; i++) {
                request_begin(s, FXP_OPENDIR);
                put_str(s, paths[i]);
                if ((st = packet_send(s))) goto done;
        }
        for (int i = 0; i < n; i++) {
                uint8_t type;
                Reader r;
                if ((st = packet_recv(s, &type, &r))) goto done;
                uint32_t rid = get_u32(&r);
                if (r.bad || rid != base + i) {
                        st = fail(s, SFTP_ERR_IO, "reply %u does not match request %u", rid, base + i);
                        goto done;
                }
                if (type == FXP_HANDLE) {
                        uint32_t len;
                        const char *data = get_str(&r, &len);
                        if (r.bad || len > sizeof h[i].data) {
                                st = fail(s, SFTP_ERR_IO, "bad HANDLE reply");
                                goto done;
                        }
                        memcpy(h[i].data, data, len);
                        h[i].len  = len;
                        open[i]   = 1;
                        status[i] = SFTP_OK;
                } else {
                        int rs = read_status(s, type, &r); // gone, not a directory, or can't
                        if (rs == SFTP_ERR_IO) {
                                st = rs;
                                goto done;
                        }
                        status[i] = rs ? rs : SFTP_FAILURE;
                }
        }

        /* Read them a few pages at a time, all the round's READDIRs in flight
         * together. BATCH per directory so a one-page directory — the common
         * case — reads its entries and the following EOF in the same round. */
        enum { BATCH = 2 };
        for (;;) {
                int m          = 0; // still-open directories this round
                uint32_t rbase = s->next_id;
                for (int i = 0; i < n; i++) {
                        if (!open[i] || eof[i]) continue;
                        order[m++] = i;
                        for (int b = 0; b < BATCH; b++) {
                                request_begin(s, FXP_READDIR);
                                put_handle(s, &h[i]);
                                if ((st = packet_send(s))) goto done;
                        }
                }
                if (m == 0) break;
                uint32_t expect = rbase;
                for (int k = 0; k < m; k++) {
                        int i = order[k];
                        for (int b = 0; b < BATCH; b++) {
                                uint8_t type;
                                Reader r;
                                if ((st = packet_recv(s, &type, &r))) goto done;
                                uint32_t rid = get_u32(&r);
                                if (r.bad || rid != expect) {
                                        st = fail(s, SFTP_ERR_IO, "reply %u does not match request %u", rid, expect);
                                        goto done;
                                }
                                ++expect;
                                if (type != FXP_NAME) {
                                        int rs = read_status(s, type, &r); // EOF, or an error: stop this one
                                        if (rs == SFTP_ERR_IO) {
                                                st = rs;
                                                goto done;
                                        }
                                        if (rs != SFTP_EOF && !eof[i]) status[i] = rs ? rs : SFTP_FAILURE;
                                        eof[i] = 1;
                                } else if (!eof[i]) {
                                        if ((st = parse_names(s, &r, &out[i]))) goto done;
                                }
                        }
                }
        }

        /* Close them all, fire-and-forget (a failed close doesn't matter) */
        for (int i = 0; i < n; i++) {
                if (!open[i]) continue;
                if ((st = close_handle_async(s, &h[i]))) goto done;
        }

done:
        free(h);
        free(open);
        free(eof);
        free(order);
        return st == SFTP_ERR_IO ? st : SFTP_OK;
}

/* List several directories at once, pipelining the requests over the one
 * connection, so a batch of directories costs a few round trips instead of a
 * few each. Returns SFTP_OK, or SFTP_ERR_IO if the connection broke. */
int
sftp_readdir_many(Sftp *s, char *const *paths, int n, SftpDir *out, int *status)
{
        for (int i = 0; i < n; i++) {
                out[i]    = (SftpDir) { 0 };
                status[i] = SFTP_OK;
        }
        for (int i = 0; i < n;) {
                int m        = 0;
                size_t bytes = 0;
                while (i + m < n && m < READDIR_MANY_DIRS && (m == 0 || bytes < READDIR_MANY_BYTES))
                        bytes += strlen(paths[i + m++]);
                int st = readdir_batch(s, paths + i, m, out + i, status + i);
                if (st) return st;
                i += m;
        }
        return SFTP_OK;
}

int
sftp_readdir(Sftp *s, const char *path, SftpDir *dir)
{
        uint32_t id = request_begin(s, FXP_OPENDIR);
        put_str(s, path);

        Handle h;
        int st = request_handle(s, id, &h);
        if (st) return st;

        /* Each READDIR returns a page of entries, until an EOF status. To
         * avoid a round trip per page, send a small batch at once (replies
         * come back in order) and read the whole batch before sending more.
         * A one-page directory — the common case — then costs a single round
         * trip for the entries plus the EOF, instead of one each. */
        enum { BATCH = 2 };
        uint32_t oldest = s->next_id; // id of the oldest READDIR without a reply
        int eof         = 0;
        st              = SFTP_OK;

        while (!eof && st == SFTP_OK) {
                for (int i = 0; i < BATCH; i++) {
                        request_begin(s, FXP_READDIR);
                        put_handle(s, &h);
                        int io = packet_send(s);
                        if (io) return io;
                }
                for (int i = 0; i < BATCH; i++) {
                        uint8_t type;
                        Reader r;
                        int io = packet_recv(s, &type, &r);
                        if (io) return io;
                        uint32_t rid = get_u32(&r);
                        if (r.bad || rid != oldest)
                                return fail(s, SFTP_ERR_IO, "reply %u does not match request %u", rid, oldest);
                        ++oldest;

                        if (type != FXP_NAME) {
                                int rs = read_status(s, type, &r);
                                if (rs == SFTP_EOF)
                                        eof = 1;
                                else if (st == SFTP_OK)
                                        st = rs ? rs : fail(s, SFTP_ERR_IO, "READDIR replied without entries");
                                continue; // drain the rest of the batch (also EOF)
                        }
                        if (eof) continue; // extra page after EOF: can't happen, but be safe
                        if ((st = parse_names(s, &r, dir))) return st;
                }
        }
        if (st == SFTP_ERR_IO) return st;

        int cst = close_handle_async(s, &h);
        return st ? st : cst;
}

void
sftp_dir_free(SftpDir *dir)
{
        Da_foreach(e, *dir)
        {
                free(e->name);
        }
        Da_destroy(dir);
}

int
sftp_remove_all(Sftp *s, const char *path)
{
        SftpAttrs a;
        int st = sftp_lstat(s, path, &a);
        if (st) return st;
        if (!is_dir(&a)) return sftp_remove(s, path);

        SftpDir dir = { 0 };
        st          = sftp_readdir(s, path, &dir);
        Da_foreach(e, dir)
        {
                if (st) break;
                char *child;
                int n = asprintf(&child, "%s/%s", path, e->name);
                assert(n != -1);
                Unused(n);
                st = sftp_remove_all(s, child);
                if (st == SFTP_NO_SUCH_FILE) st = SFTP_OK; // gone meanwhile
                free(child);
        }
        sftp_dir_free(&dir);
        return st ? st : sftp_rmdir(s, path);
}
