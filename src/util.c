#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

int verbose;

const char *
pathjoin(const char *dir, const char *base)
{
        assert(dir && base);
        int dirlen      = strlen(dir);
        const char *sep = (dirlen > 0 && dir[dirlen - 1] == '/') ? "" : "/";
        int len         = dirlen + strlen(sep) + strlen(base) + 1;
        char *buf       = malloc(len);
        assert(buf);
        int n = snprintf(buf, len, "%s%s%s", dir, sep, base);
        assert(n == len - 1);
        Unused(n);
        return buf;
}

/* DIR joined with REL, which may be "" */
const char *
root_join(const char *dir, const char *rel)
{
        return *rel ? pathjoin(dir, rel) : strdup(dir);
}

/* Is PATH inside DIR? Everything is inside "". */
int
inside(const char *path, const char *dir)
{
        size_t n = strlen(dir);
        return n == 0 || (!strncmp(path, dir, n) && path[n] == '/');
}

const char *
rel_path(const char *dir, const char *path)
{
        const char *rel = path + strlen(dir);
        while (*rel == '/')
                ++rel;
        return rel;
}

int
mkdir_p(const char *path)
{
        char *p = strdup(path);
        int r   = 0;
        for (char *slash = p + 1; r == 0 && (slash = strchr(slash, '/')); slash++) {
                *slash = 0;
                if (mkdir(p, 0777) == -1 && errno != EEXIST) r = -1;
                *slash = '/';
        }
        if (r == 0 && mkdir(p, 0777) == -1 && errno != EEXIST) r = -1;
        free(p);
        return r;
}

uint64_t
hash_str(uint64_t hash, const char *s)
{
        for (;; s++) {
                hash = (hash ^ (unsigned char) *s) * 0x100000001b3;
                if (*s == 0) return hash;
        }
}

char *
state_path(const char *name)
{
        char dir[PATH_MAX];
        const char *state = getenv("XDG_STATE_HOME");
        if (state && *state)
                snprintf(dir, sizeof dir, "%s/isf", state);
        else
                snprintf(dir, sizeof dir, "%s/.local/state/isf", getenv("HOME") ? getenv("HOME") : ".");
        if (mkdir_p(dir) == -1) {
                LOG_ERR("Cannot create '%s'", dir);
                return NULL;
        }
        return (char *) pathjoin(dir, name);
}

int
path_safe(const char *rel)
{
        if (rel[0] == '/') return 0; // absolute
        if (rel[0] == 0) return 1;   // the root itself
        for (const char *p = rel;;) {
                const char *slash = strchr(p, '/');
                size_t n          = slash ? (size_t) (slash - p) : strlen(p);
                if (n == 0) return 0;                               // empty component: "//", leading or trailing /
                if (n == 1 && p[0] == '.') return 0;                // "."
                if (n == 2 && p[0] == '.' && p[1] == '.') return 0; // ".."
                if (!slash) return 1;
                p = slash + 1;
        }
}

int
name_safe(const char *name)
{
        return name[0] != 0 && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

const char *
temp_path(const char *dir, int worker)
{
        char name[40];
        snprintf(name, sizeof name, ".isf.%ld.%d.tmp", (long) getpid(), worker);
        return pathjoin(dir, name);
}

int
is_temp_name(const char *name)
{
        /* ".isf.<pid>.tmp", and the older ".isf.tmp" (length 8) too */
        size_t n = strlen(name);
        return n >= 8 && !strncmp(name, ".isf.", 5) && !strcmp(name + n - 4, ".tmp");
}
