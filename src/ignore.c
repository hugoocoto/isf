#define _DEFAULT_SOURCE

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ignore.h"
#include "util.h"

/* Editor temp files: vim swap files and its 4913 test file, backups~, emacs
 * locks */
static const char *defaults[] = { "*.swp", "*.swx", "*~", "4913", ".#*" };

static void
add(Ignore *ign, const char *line)
{
        char *p   = strdup(line);
        size_t n  = strlen(p);
        Pattern t = { 0 };
        if (n > 1 && p[n - 1] == '/') {
                t.dir_only = 1;
                p[--n]     = 0;
        }
        t.anchored = strchr(p, '/') != NULL;
        t.pattern  = strdup(p[0] == '/' ? p + 1 : p);
        free(p);
        Da_append(ign, t);
}

void
ignore_load(Ignore *ign, const char *dir)
{
        Da_foreach(t, *ign)
        {
                free(t->pattern);
        }
        ign->count = 0;
        for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++)
                add(ign, defaults[i]);

        const char *path = pathjoin(dir, IGNORE_FILE);
        FILE *f          = fopen(path, "r");
        if (f) {
                char *line = NULL;
                size_t cap = 0;
                ssize_t n;
                while ((n = getline(&line, &cap, f)) != -1) {
                        while (n > 0 && isspace((unsigned char) line[n - 1]))
                                line[--n] = 0;
                        if (n == 0 || line[0] == '#') continue;
                        if (line[0] == '!')
                                LOG_WARN("'%s': \"!\" isn't supported, skipping '%s'", path, line);
                        else
                                add(ign, line);
                }
                free(line);
                fclose(f);
        }
        free((void *) path);
}

static int
matches(const Ignore *ign, const char *path, int is_dir)
{
        const char *name = strrchr(path, '/');
        name             = name ? name + 1 : path;
        Da_foreach(t, *ign)
        {
                if (t->dir_only && !is_dir) continue;
                if (fnmatch(t->pattern, t->anchored ? path : name, FNM_PATHNAME) == 0) return 1;
        }
        return 0;
}

int
ignored(const Ignore *ign, const char *rel, int is_dir)
{
        /* Each directory on the way, then REL itself */
        char *copy = strdup(rel);
        int hit    = 0;
        for (char *slash = copy;; slash++) {
                slash = strchr(slash, '/');
                if (slash) *slash = 0;
                hit = matches(ign, copy, slash ? 1 : is_dir);
                if (hit || slash == NULL) break;
                *slash = '/';
        }
        free(copy);
        return hit;
}
