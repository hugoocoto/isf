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
/* List N directories at once, pipelined. OUT[i] and STATUS[i] (caller-provided
 * arrays) get PATHS[i]'s entries and how listing it went: SFTP_NO_SUCH_FILE if
 * it's gone or isn't a directory (then it's empty), another code if it couldn't
 * be listed (then OUT[i] may be partial). Free each OUT[i] with sftp_dir_free.
 * Returns SFTP_OK, or SFTP_ERR_IO. */
int sftp_readdir_many(Sftp *s, char *const *paths, int n, SftpDir *out, int *status);
void sftp_dir_free(SftpDir *dir);

#endif // !SFTP_H_
