#ifndef IGNORE_H_
#define IGNORE_H_

#include "cum.h"

/* Paths that aren't synced: editor temp files, and the patterns in the
 * folder's .isfignore. One pattern per line, like .gitignore: "#" starts a
 * comment, a "/" at the start or in the middle ties it to the folder (else it
 * matches names at any depth), a "/" at the end matches only directories, and
 * "*", "?" and "[...]" work as in the shell. "!" isn't supported. */

#define IGNORE_FILE ".isfignore"

typedef struct {
        char *pattern;
        int anchored; // matches the whole path, not a name at any depth
        int dir_only; // matches only directories
} Pattern;

typedef Da(Pattern) Ignore;

/* Load the defaults and DIR/.isfignore into IGN, replacing what it had */
void ignore_load(Ignore *ign, const char *dir);

/* Is REL, a path inside the folder, ignored? Or one of the directories it is
 * in? IS_DIR: REL itself is a directory. */
int ignored(const Ignore *ign, const char *rel, int is_dir);

#endif // !IGNORE_H_
