#ifndef UTIL_H_
#define UTIL_H_

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cum.h"

/* isf's version, from git describe (see the Makefile) */
#ifndef VERSION
#define VERSION "unknown"
#endif

/* -v: show every event, and where errors come from */
extern int verbose;

/* How many errors were said so far (by any thread): read it with
 * logged_errors() */
extern int log_errors;
static inline int
logged_errors(void)
{
        return __atomic_load_n(&log_errors, __ATOMIC_RELAXED);
}

/* fprintf to F, stdout or stderr, from any thread, under the status line if
 * there is one (see status_line): lines printed with it scroll above it. */
__attribute__((format(printf, 2, 3))) void say(FILE *f, const char *fmt, ...);

/* Keep TEXT on the last line of the terminal, under what's said, until it's
 * changed or NULL takes it away. Only if stdout is a terminal. */
void status_line(const char *text);

/* Errors and warnings go to stderr, with where they come from if verbose */
#define LOG(kind, fmt, ...)                                                         \
        do {                                                                        \
                if ((kind)[0] == 'E') __atomic_fetch_add(&log_errors, 1, __ATOMIC_RELAXED); \
                if (verbose)                                                        \
                        say(stderr, kind " at %s (" __FILE__ ":%d): " fmt "\n",     \
                            __func__, __LINE__, ##__VA_ARGS__);                     \
                else                                                                \
                        say(stderr, "isf: " fmt "\n", ##__VA_ARGS__);               \
        } while (0)

/* printf, only if verbose */
#define VPRINT(...)                                    \
        do {                                           \
                if (verbose) say(stdout, __VA_ARGS__); \
        } while (0)

#define LOG_WARN(fmt, ...) LOG("Warning", fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) LOG("Error", fmt ": %s", ##__VA_ARGS__, strerror(errno))

typedef Da(char) CharBuf;

/* DIR/BASE, malloc'd */
const char *pathjoin(const char *dir, const char *base);
/* DIR joined with REL, which may be "" (then a copy of DIR), malloc'd */
const char *root_join(const char *dir, const char *rel);
/* Is PATH inside DIR? Everything is inside "". */
int inside(const char *path, const char *dir);

/* Is REL a safe relative path inside a root: "" or '/'-separated components,
 * none empty, "." or ".."? Rejects anything (absolute, "..") that could reach
 * outside the root. Paths from the remote go through this before they build a
 * local path. */
int path_safe(const char *rel);
/* Is NAME a safe single directory-entry name: not empty, ".", "..", and with
 * no '/'? For names from a remote listing. */
int name_safe(const char *name);

/* Make a temp file in DIR and open it for writing: a new one, never one
 * that's there already (someone could leave a symlink in its place, and the
 * transfer would write through it). *PATH gets its name, to free. Returns the
 * fd, or -1 (errno set). */
int temp_create(const char *dir, char **path);
/* Path of a temp file for isf to transfer through, in DIR
 * (".isf.<pid>.<n>.tmp"): the pid keeps concurrent runs apart, N (a new one
 * each time) the transfers of this one. malloc'd. */
const char *temp_path(const char *dir);
/* Is NAME one of isf's temp files (".isf.<pid>.tmp", from any run)? They are
 * never synced. */
int is_temp_name(const char *name);
/* The part of PATH after DIR, which it starts with: "" for DIR itself */
const char *rel_path(const char *dir, const char *path);

/* mkdir -p. Returns 0, or -1 with errno set. */
int mkdir_p(const char *path);

/* FNV-1a of S and its NUL, going on from HASH: start with HASH_INIT */
#define HASH_INIT 0xcbf29ce484222325ull
uint64_t hash_str(uint64_t hash, const char *s);

/* NAME in isf's state directory, $XDG_STATE_HOME/isf or ~/.local/state/isf,
 * which is created if needed. malloc'd, NULL (logged) on error. */
char *state_path(const char *name);

#endif // !UTIL_H_
