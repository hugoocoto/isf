#ifndef SFTP_H_
#define SFTP_H_

#include <stdint.h>
#include <sys/types.h>

#include "cum.h"

/* Minimal SFTP v3 client (draft-ietf-secsh-filexfer-02). It runs
 * `ssh -s HOST sftp` once and talks to the remote sftp-server through the
 * ssh process's stdin/stdout, so ~/.ssh/config, keys and known_hosts are all
 * handled by ssh. Most requests are synchronous, one in flight at a time;
 * transfers and sftp_readdir_many pipeline theirs. */

/* Status codes sent by the server (SSH_FXP_STATUS). Functions below return one
 * of these, SFTP_OK on success. */
enum {
        SFTP_OK                = 0,
        SFTP_EOF               = 1,
        SFTP_NO_SUCH_FILE      = 2,
        SFTP_PERMISSION_DENIED = 3,
        SFTP_FAILURE           = 4,
        SFTP_BAD_MESSAGE       = 5,
        SFTP_NO_CONNECTION     = 6,
        SFTP_CONNECTION_LOST   = 7,
        SFTP_OP_UNSUPPORTED    = 8,
};

/* Returned instead of a status code when the connection itself failed (ssh
 * exited, short read, malformed reply). The session can't be used after it. */
#define SFTP_ERR_IO (-1)

/* sftp_put_many and sftp_get_many: the remote file isn't what was expected
 * anymore, so it was left alone */
#define SFTP_CHANGED (-2)

/* Bits of SftpAttrs.flags telling which fields are set */
#define SFTP_ATTR_SIZE 0x00000001
#define SFTP_ATTR_UIDGID 0x00000002
#define SFTP_ATTR_PERMISSIONS 0x00000004
#define SFTP_ATTR_ACMODTIME 0x00000008
#define SFTP_ATTR_EXTENDED 0x80000000

typedef struct {
        uint32_t flags;
        uint64_t size;
        uint32_t uid, gid;
        uint32_t perm; // mode bits, including the file type (S_IFDIR...)
        uint32_t atime, mtime;
} SftpAttrs;

typedef struct {
        pid_t pid;        // ssh process
        int to;           // ssh stdin
        int from;         // ssh stdout
        uint32_t next_id; // id of the next request
        int posix_rename; // server supports posix-rename@openssh.com
        int dead;         // the connection broke (an SFTP_ERR_IO happened)
        int pending;      // fire-and-forget replies to drain before the next read
        Da(unsigned char) buf;
        char error[256]; // why the last call failed
} Sftp;

typedef struct {
        char *name;
        SftpAttrs attrs;
} SftpEntry;

/* An open remote file or directory */
typedef struct {
        char data[256]; // the spec caps handles at 256 bytes
        uint32_t len;
} SftpHandle;

typedef Da(SftpEntry) SftpDir;

/* PORT may be NULL to let ssh pick it (default or ~/.ssh/config). OPTS are
 * more ssh options, NULL terminated, or NULL. */
int sftp_connect(Sftp *s, const char *host, const char *port, char *const *opts);

/* Run ARGV (NULL terminated) with its stdin and stdout on pipes: write to
 * *TO, read from *FROM. Returns the pid, or -1 with errno set. */
pid_t spawn_piped(char *const argv[], int *to, int *from);
void sftp_disconnect(Sftp *s);

/* sftp_stat follows symlinks, sftp_lstat doesn't. */
int sftp_stat(Sftp *s, const char *path, SftpAttrs *attrs);
int sftp_lstat(Sftp *s, const char *path, SftpAttrs *attrs);
/* Set the fields of ATTRS that are in ATTRS->flags. */
int sftp_setstat(Sftp *s, const char *path, const SftpAttrs *attrs);

int sftp_mkdir(Sftp *s, const char *path);
/* Create PATH and any missing parents. OK if it already is a directory. */
int sftp_mkdir_p(Sftp *s, const char *path);
int sftp_rmdir(Sftp *s, const char *path);
/* Remove a file or a symlink. */
int sftp_remove(Sftp *s, const char *path);
/* Remove PATH, and everything inside if it's a directory. Returns
 * SFTP_NO_SUCH_FILE if there was nothing to remove. */
int sftp_remove_all(Sftp *s, const char *path);
/* Replaces TO if it exists (atomically if the server has posix-rename). */
int sftp_rename(Sftp *s, const char *from, const char *to);
/* Create LINK pointing to TARGET. */
int sftp_symlink(Sftp *s, const char *target, const char *link);

/* Where the symlink PATH points, into TARGET of SIZE bytes (NUL terminated). */
int sftp_readlink(Sftp *s, const char *path, char *target, size_t size);

/* Write everything read from FD into PATH, creating or truncating it, then
 * set ATTRS on it (if not NULL). */
int sftp_put(Sftp *s, int fd, const char *path, const SftpAttrs *attrs);

/* Write the contents of PATH into FD, at the same offsets. */
int sftp_get(Sftp *s, const char *path, int fd);

/* Append PATH's entries to DIR, without "." and "..". Free them with
 * sftp_dir_free, also after an error. */
int sftp_readdir(Sftp *s, const char *path, SftpDir *dir);
/* The same, and SELF gets the directory's own attributes, from its "." entry,
 * if the server lists it (then *FOUND is set; OpenSSH's does) */
int sftp_readdir_self(Sftp *s, const char *path, SftpDir *dir, SftpAttrs *self, int *found);
/* List N directories at once, pipelined. OUT[i] and STATUS[i] (caller-provided
 * arrays) get PATHS[i]'s entries and how listing it went: SFTP_NO_SUCH_FILE if
 * it's gone or isn't a directory (then it's empty), another code if it couldn't
 * be listed (then OUT[i] may be partial). Free each OUT[i] with sftp_dir_free.
 * Returns SFTP_OK, or SFTP_ERR_IO. */
int sftp_readdir_many(Sftp *s, char *const *paths, int n, SftpDir *out, int *status);
void sftp_dir_free(SftpDir *dir);

/* A file in a batch of sftp_put_many or sftp_get_many */
typedef struct {
        int fd;             // the local file: read from (put) or written to (get)
        const char *path;   // put: the temp file it's written to; get: the file read
        const char *target; // put: the name it gets once written
        SftpAttrs attrs;    // put: set on it before (flags 0: nothing)
        char expect;        // put: TARGET is replaced only if it's still this: 0
                            // nothing, 'f' a file with EXPECT_ATTRS's size, mtime and
                            // mode, 'd' a directory, 'l' a symlink, '-' anything
        SftpAttrs expect_attrs;
        uint64_t size;      // get: how much to read (a file shorter than that changed);
                            // put: how big it is, as far as known (to budget)

        /* What happened */
        int status;      // SFTP_OK, SFTP_CHANGED, or what failed
        int at;          // put: where it stopped, SFTP_AT_*
        char error[256]; // why it failed

        /* Internal */
        SftpHandle h;
        int state, eof, waiting, reads;
        uint64_t offset, retry_offset;
        uint32_t retry_len;
} SftpFile;

enum {
        SFTP_AT_OPEN = 1, // the temp file couldn't be made: SFTP_NO_SUCH_FILE, its folder is missing
        SFTP_AT_WRITE,    // writing it, or setting ATTRS (it's removed)
        SFTP_AT_CHECK,    // TARGET wasn't EXPECT, or couldn't be checked (it's removed)
        SFTP_AT_RENAME,   // it's written, but not renamed: the temp file is still there.
                          // SFTP_OP_UNSUPPORTED: the server can't replace TARGET in one step
};

/* Bytes of file data sent (and acknowledged) and received so far by
 * sftp_put_many and sftp_get_many, on every connection: read it with
 * __atomic_load_n, it's added to from any thread */
extern uint64_t sftp_moved;

/* Send N local files, all at once: each into its temp file PATH, with ATTRS,
 * then, if TARGET is still EXPECT, renamed over it. A NULL TARGET leaves the
 * file in PATH, for the caller to put in place (isf has the agent do that,
 * where nothing can change in between). Returns SFTP_OK, or SFTP_ERR_IO if
 * the connection broke; how each file went is in its STATUS and AT. A batch
 * of small files takes about the round trips of one. */
int sftp_put_many(Sftp *s, SftpFile *files, int n);

/* Fetch N remote files, all at once: the first SIZE bytes of each PATH into
 * its FD, at the same offsets. Returns SFTP_OK or SFTP_ERR_IO, and each file's
 * STATUS. */
int sftp_get_many(Sftp *s, SftpFile *files, int n);

/* A request of sftp_batch */
enum { SFTP_OP_LSTAT, SFTP_OP_SETSTAT, SFTP_OP_MKDIR, SFTP_OP_RMDIR, SFTP_OP_REMOVE, SFTP_OP_RENAME };
typedef struct {
        int op;           // SFTP_OP_*
        const char *path;
        const char *to;   // RENAME: PATH's new name, which mustn't exist
        SftpAttrs attrs;  // SETSTAT, MKDIR: to set (flags 0: none); LSTAT: what it found
        int status;       // out: SFTP_OK, or the server's status
        char error[256];  // out: why it failed
} SftpOp;

/* Send the N requests OPS all at once, and read every reply into them. The
 * server does them in order, so one may depend on one before it (a MKDIR on
 * its parent's). Returns SFTP_OK, or SFTP_ERR_IO if the connection broke. */
int sftp_batch(Sftp *s, SftpOp *ops, int n);

#endif // !SFTP_H_
