#ifndef AGENT_H_
#define AGENT_H_

#include <stdint.h>
#include <sys/types.h>

#include "sync.h"
#include "util.h"

/* The agent is isf running on the remote (`isf --agent FOLDERS...`): it
 * watches the folders and reports what changes. A message is its type, the
 * folder index as a byte, a number as 16 hex digits, then text, NUL
 * terminated:
 *   'L' at the start, before 'R': what's in a folder
 *   'R' ready: AGENT_PROTOCOL, and isf's version
 *   'C' a file was written, made, removed or moved out, not by isf
 *   'W' isf wrote a file: renamed it from one of its temp files
 *   'A' only a file's attributes changed (chmod, touch, isf's SETSTATs)
 *   'D' a directory changed
 *   'M' a file or directory was renamed, inside the folder
 *   'O' events were lost, check everything: 0, no text
 * The number of C, W, A and D is 0. Their text is what lstat says of the path
 * now, in hex: size (16 digits), mtime (8) and mode with the type (8; all 0 if
 * it's gone), then the path relative to the folder. M's number is the length
 * of the old path, and its text what lstat says of the new one, then the old
 * path, then the new one. L's number is the length of the folder's path, and
 * its text what lstat says of the folder, its path, then for each entry what
 * lstat says of it, its name and a '/'. A big folder takes several L messages
 * in a row, each with some of its entries. The folders ignored there, and past
 * the first LIST_MAX entries, aren't listed.
 * Protocol 1 (isf before versions) had no number, and no text in 'R'. Both
 * sides have to speak the same one. */
#define AGENT_PROTOCOL 3

/* Remote side: watch ROOTS and report changes on stdout until stdin closes,
 * which means the other side is gone */
int agent_main(Root *roots, int count);

/* Local side: the agent, reached through ssh */
typedef struct {
        pid_t pid;
        int to;      // its stdin: closing it stops the agent
        int from;    // its stdout: the messages
        int ready;     // got 'R': it's watching
        int protocol;  // its AGENT_PROTOCOL, from 'R'
        char *version; // its isf version, from 'R' (NULL for protocol 1)
        CharBuf buf;   // read, but not a whole message yet
} Agent;

/* Run `ISF --agent` on HOST for the remote folders of ROOTS. SSH_OPTS (NULL
 * terminated) and PORT (may be NULL) go to ssh. Returns 1 (logged) on error. */
int agent_start(Agent *a, const char *isf, const char *host, const char *port,
                char *const *ssh_opts, const Root *roots, int count);

/* Read what the agent sent. 'R' sets A->ready, A->protocol and A->version;
 * 'M' goes to MOVED, 'L' to LISTED (which takes DIR: free it with
 * sftp_dir_free; SELF and DIR are NULL if the message was damaged), and the
 * other messages to HANDLE, with what the agent saw of
 * the path as a remote State (no link target), or NULL for 'O'. Returns 1 if
 * the agent is gone: then nothing on the remote is seen anymore. */
int agent_read(Agent *a, void (*handle)(char type, int root, const char *rel, const State *seen),
               void (*moved)(int root, const char *from, const char *to, const State *seen),
               void (*listed)(int root, const char *rel, const SftpAttrs *self, SftpDir *dir));

/* Stop the agent and wait for it. Returns its exit status, or -1. */
int agent_stop(Agent *a);

#endif // !AGENT_H_
