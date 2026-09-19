#ifndef AGENT_H_
#define AGENT_H_

#include <sys/types.h>

#include "sync.h"
#include "util.h"

/* The agent is isf running on the remote (`isf --agent FOLDERS...`): it
 * watches the folders and reports what changes. A message is its type ('R'
 * ready, 'C' changed, 'D' a directory changed, 'O' overflow: check
 * everything), the folder index as a byte, then the path relative to that
 * folder, NUL terminated. */

/* Remote side: watch ROOTS and report changes on stdout until stdin closes,
 * which means the other side is gone */
int agent_main(Root *roots, int count);

/* Local side: the agent, reached through ssh */
typedef struct {
        pid_t pid;
        int to;      // its stdin: closing it stops the agent
        int from;    // its stdout: the messages
        int ready;   // got 'R': it's watching
        CharBuf buf; // read, but not a whole message yet
} Agent;

/* Run `ISF --agent` on HOST for the remote folders of ROOTS. SSH_OPTS (NULL
 * terminated) and PORT (may be NULL) go to ssh. Returns 1 (logged) on error. */
int agent_start(Agent *a, const char *isf, const char *host, const char *port,
                char *const *ssh_opts, const Root *roots, int count);

/* Read what the agent sent. 'R' sets A->ready, the other messages go to
 * HANDLE. Returns 1 if the agent is gone: then nothing on the remote is seen
 * anymore. */
int agent_read(Agent *a, void (*handle)(char type, int root, const char *rel));

/* Stop the agent and wait for it. Returns its exit status, or -1. */
int agent_stop(Agent *a);

#endif // !AGENT_H_
