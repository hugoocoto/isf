#define _DEFAULT_SOURCE

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agent.h"
#include "sftp.h"
#include "watch.h"

/* Remote side: the folders it watches */
static struct {
        Root *roots;
        int count;
} g;

/* Tell the other side about a change */
static void
agent_send(char type, int root, const char *rel)
{
        size_t len = strlen(rel) + 3;
        char *msg  = malloc(len);
        assert(msg);
        msg[0] = type;
        msg[1] = root;
        memcpy(msg + 2, rel, len - 2);
        for (size_t done = 0; done < len;) {
                ssize_t n = write(STDOUT_FILENO, msg + done, len - done);
                if (n == -1 && errno == EINTR) continue;
                if (n == -1) exit(0); // the other side is gone
                done += n;
        }
        free(msg);
}

/* The agent's handle_event: report what changed, don't sync anything */
static void
agent_event(const struct inotify_event *event, int fd)
{
        if (event->mask & IN_Q_OVERFLOW) {
                for (int i = 0; i < g.count; i++)
                        agent_send('O', i, "");
                return;
        }

        Watch *w = find_watch(event->wd);
        if (w == NULL) return;
        int root_i       = w->root;
        const Root *root = &g.roots[root_i];

        if (event->mask & IN_IGNORED) {
                forget_watch(w);
                return;
        }
        if (event->len == 0) {
                /* Without its folder the other side would see everything as
                 * deleted: stop, so it stops too */
                if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) && !strcmp(w->path, root->local)) {
                        LOG_WARN("'%s' was removed or moved, stopping", w->path);
                        exit(1);
                }
                return;
        }
        if (is_temp_name(event->name)) return; // a transfer in progress

        const char *path = pathjoin(w->path, event->name);
        if ((event->mask & (IN_CREATE | IN_MOVED_TO)) && (event->mask & IN_ISDIR))
                listen_folder(path, root_i, fd);
        if (event->mask & (IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO))
                agent_send(event->mask & IN_ISDIR ? 'D' : 'C', root_i, rel_path(root->local, path));
        free((void *) path);
}

int
agent_main(Root *roots, int count)
{
        g.roots = roots;
        g.count = count;

        int fd = watch_init();
        if (fd < 0) return 1;
        for (int i = 0; i < count; i++) {
                if (listen_folder(roots[i].local, i, fd)) return 1;
        }
        agent_send('R', 0, "");

        struct pollfd fds[] = {
                { .fd = fd, .events = POLLIN },
                { .fd = STDIN_FILENO, .events = POLLIN },
        };
        for (;;) {
                if (poll(fds, 2, -1) == -1) {
                        if (errno == EINTR) continue;
                        LOG_ERR("poll");
                        return 1;
                }
                if (fds[1].revents) return 0;
                if ((fds[0].revents & POLLIN) && handle_events(fd, agent_event)) return 1;
        }
}

/* Append S to B in single quotes, for the remote shell */
static void
append_quoted(CharBuf *b, const char *s)
{
        Da_append(b, '\'');
        for (; *s; s++) {
                if (*s == '\'') {
                        for (const char *q = "'\\''"; *q; q++)
                                Da_append(b, *q);
                } else {
                        Da_append(b, *s);
                }
        }
        Da_append(b, '\'');
}

int
agent_start(Agent *a, const char *isf, const char *host, const char *port,
            char *const *ssh_opts, const Root *roots, int count)
{
        *a = (Agent) { .pid = -1, .to = -1, .from = -1 };

        /* ssh sends the command to the remote shell as one string */
        CharBuf cmd = { 0 };
        for (const char *p = isf; *p; p++)
                Da_append(&cmd, *p); // as given, so ~ still expands
        for (const char *p = " --agent"; *p; p++)
                Da_append(&cmd, *p);
        for (int i = 0; i < count; i++) {
                Da_append(&cmd, ' ');
                append_quoted(&cmd, roots[i].remote);
        }
        Da_append(&cmd, 0);

        Command c = { 0 };
        Command_add(&c, "ssh", "-x", "-a", "-oClearAllForwardings=yes", "-oPermitLocalCommand=no",
                    "-oRemoteCommand=none", "-oRequestTTY=no");
        for (; ssh_opts && *ssh_opts; ssh_opts++)
                Command_add(&c, *ssh_opts);
        if (port) Command_add(&c, "-p", port);
        Command_add(&c, "--", host, cmd.items, NULL);

        a->pid = spawn_piped(c.items, &a->to, &a->from);
        Command_destroy(&c);
        Da_destroy(&cmd);
        if (a->pid == -1) {
                LOG_ERR("Cannot run ssh");
                return 1;
        }
        return 0;
}

int
agent_read(Agent *a, void (*handle)(char type, int root, const char *rel))
{
        char chunk[4096];
        ssize_t n = read(a->from, chunk, sizeof chunk);
        if (n == -1 && errno == EINTR) return 0;
        if (n <= 0) return 1;
        for (ssize_t i = 0; i < n; i++)
                Da_append(&a->buf, chunk[i]);

        /* A message is type, root, path and NUL: at least 3 bytes */
        char *b      = a->buf.items;
        size_t start = 0, count = a->buf.count;
        while (count - start >= 3) {
                char *nul = memchr(b + start + 2, 0, count - start - 2);
                if (nul == NULL) break;
                if (b[start] == 'R')
                        a->ready = 1;
                else
                        handle(b[start], (unsigned char) b[start + 1], b + start + 2);
                start = nul - b + 1;
        }
        memmove(b, b + start, count - start);
        a->buf.count = count - start;
        return 0;
}

int
agent_stop(Agent *a)
{
        int status = -1;
        if (a->to != -1) close(a->to); // EOF on its stdin stops it
        if (a->from != -1) close(a->from);
        if (a->pid > 0 && waitpid(a->pid, &status, 0) > 0)
                status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        Da_destroy(&a->buf);
        *a = (Agent) { .pid = -1, .to = -1, .from = -1 };
        return status;
}
