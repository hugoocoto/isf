/* Unit tests for the parts that don't need a remote */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ignore.h"
#include "plan.h"
#include "util.h"

static int failed;

#define CHECK(cond)                                                        \
        do {                                                               \
                if (!(cond)) {                                             \
                        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
                                #cond);                                    \
                        failed = 1;                                        \
                }                                                          \
        } while (0)

static void
test_paths(void)
{
        CHECK(path_safe(""));
        CHECK(path_safe("a"));
        CHECK(path_safe("a/b/c"));
        CHECK(path_safe("..a/b.."));
        CHECK(!path_safe("/a"));
        CHECK(!path_safe("a/"));
        CHECK(!path_safe("a//b"));
        CHECK(!path_safe("./a"));
        CHECK(!path_safe("a/../b"));
        CHECK(!path_safe(".."));

        CHECK(name_safe("a"));
        CHECK(name_safe("..."));
        CHECK(!name_safe(""));
        CHECK(!name_safe("."));
        CHECK(!name_safe(".."));
        CHECK(!name_safe("a/b"));

        CHECK(inside("a/b", "a"));
        CHECK(inside("a/b/c", "a/b"));
        CHECK(inside("anything", ""));
        CHECK(!inside("a", "a"));
        CHECK(!inside("ab", "a"));
        CHECK(!inside("a-b/c", "a"));

        CHECK(is_temp_name(".isf.123.0.tmp"));
        CHECK(is_temp_name(".isf.tmp"));
        CHECK(!is_temp_name(".isf.tmpx"));
        CHECK(!is_temp_name("isf.1.tmp"));
}

static void
test_ignore(void)
{
        char dir[] = "/tmp/isf-unit.XXXXXX";
        CHECK(mkdtemp(dir));
        const char *file = pathjoin(dir, IGNORE_FILE);
        FILE *f          = fopen(file, "w");
        CHECK(f);
        fputs("# comment\n*.log\nbuild/\n/top.txt\ndocs/*.tmp\n!neg\n", f);
        fclose(f);

        Ignore ign = { 0 };
        ignore_load(&ign, dir);
        CHECK(ignored(&ign, "a.log", 0));
        CHECK(ignored(&ign, "x/y/a.log", 0));
        CHECK(ignored(&ign, "build", 1));
        CHECK(!ignored(&ign, "build", 0));
        CHECK(ignored(&ign, "x/build", 1));
        CHECK(ignored(&ign, "x/build/y.c", 0)); // inside an ignored directory
        CHECK(ignored(&ign, "top.txt", 0));
        CHECK(!ignored(&ign, "x/top.txt", 0));
        CHECK(ignored(&ign, "docs/a.tmp", 0));
        CHECK(!ignored(&ign, "x/docs/a.tmp", 0));
        CHECK(!ignored(&ign, "docs/x/a.tmp", 0));
        CHECK(!ignored(&ign, "neg", 0));
        CHECK(!ignored(&ign, "# comment", 0));
        CHECK(ignored(&ign, "a.swp", 0)); // defaults
        CHECK(ignored(&ign, "x/a~", 0));
        CHECK(ignored(&ign, "4913", 0));
        CHECK(ignored(&ign, ".#lock", 0));
        CHECK(!ignored(&ign, "a.c", 0));

        unlink(file);
        rmdir(dir);
        free((void *) file);
}

/* A file: SIZE bytes, MTIME, MODE, and local ctime STAMP */
static State
file(uint64_t size, uint32_t mtime, uint32_t mode, uint64_t stamp)
{
        return (State) { .type = 'f', .size = size, .mtime = mtime, .mode = mode, .stamp = stamp };
}

static State
dir(uint32_t mode)
{
        return (State) { .type = 'd', .mode = mode };
}

/* Plan one file and check it gives one step of TYPE, UP, CONFLICT (or none
 * if TYPE is -1) */
static int
plans(State L, State M, State R, int what, int type, int up, int conflict)
{
        Plan p = { 0 };
        plan_file(&p, "x", &L, &M, &R, what);
        int ok = type == -1 ? p.count == 0 :
                              p.count == 1 && (int) p.items[0].type == type && p.items[0].up == up &&
                                      p.items[0].conflict == conflict;
        if (!ok && p.count)
                fprintf(stderr, "  got %d steps, the first %d up %d conflict %d\n", p.count, p.items[0].type,
                        p.items[0].up, p.items[0].conflict);
        plan_free(&p);
        return ok;
}

static void
test_plan(void)
{
        State none = { 0 };
        State f1   = file(10, 100, 0644, 1);
        State f1b  = file(10, 100, 0644, 2); // same, but its ctime moved: edited in the same second
        State f2   = file(20, 200, 0644, 3);
        State f3   = file(30, 300, 0644, 4);
        State f1x  = file(10, 100, 0755, 5); // only the mode changed
        State f1t  = file(10, 150, 0600, 6); // same size, mode and times changed

        CHECK(plans(f1, f1, f1, SYNC_DATA, -1, 0, 0));                 // nothing changed
        CHECK(plans(f2, f1, f1, SYNC_DATA, ACT_COPY, 1, 0));           // edited here
        CHECK(plans(f1, f2, f1, SYNC_DATA, ACT_COPY, 0, 0));           // edited there
        CHECK(plans(f1b, f1, f1, SYNC_DATA, ACT_COPY, 1, 0));          // same second, same size
        CHECK(plans(f2, f2, f1, SYNC_DATA, ACT_RECORD, 0, 0));         // both the same way
        CHECK(plans(f3, f2, f1, SYNC_DATA, ACT_COPY, 1, 1));           // both: here is newer
        CHECK(plans(f2, f3, f1, SYNC_DATA, ACT_COPY, 0, 1));           // both: there is newer
        CHECK(plans(none, f1, f1, SYNC_DATA, ACT_REMOVE, 1, 0));       // deleted here
        CHECK(plans(f1, none, f1, SYNC_DATA, ACT_REMOVE, 0, 0));       // deleted there
        CHECK(plans(none, f2, f1, SYNC_DATA, ACT_COPY, 0, 0));         // deleted here, edited there
        CHECK(plans(f2, none, f1, SYNC_DATA, ACT_COPY, 1, 0));         // edited here, deleted there
        CHECK(plans(none, none, f1, SYNC_DATA, ACT_RECORD, 0, 0));     // deleted on both
        CHECK(plans(f1, none, none, SYNC_DATA, ACT_COPY, 1, 0));       // new here
        CHECK(plans(none, f1, none, SYNC_DATA, ACT_COPY, 0, 0));       // new there
        CHECK(plans(f1t, f1, f1, SYNC_ATTR, ACT_ATTRS, 1, 0));         // chmod/touch here
        CHECK(plans(f1t, f1, f1, SYNC_DATA, ACT_COPY, 1, 0));          // the same, from a write
        CHECK(plans(f1, f1x, f1, SYNC_DATA, ACT_ATTRS, 0, 0));         // chmod there
        CHECK(plans(f1t, f2, f1, SYNC_ATTR, ACT_COPY, 0, 1));          // a conflict is never just attributes

        State d1 = dir(0755), d2 = dir(0700);
        CHECK(decide_dir(&d1, &d1, &d1) == DIR_BOTH);
        CHECK(decide_dir(&d1, &none, &d1) == DIR_GONE);     // deleted there
        CHECK(decide_dir(&none, &d1, &d1) == DIR_GONE);     // deleted here
        CHECK(decide_dir(&d1, &none, &none) == DIR_WINS);   // new here
        CHECK(decide_dir(&d1, &f1, &d1) == DIR_LOST);       // replaced by a file there
        CHECK(decide_dir(&d2, &f1, &d1) == DIR_WINS);       // ... but its mode changed here
        CHECK(decide_dir(&d1, &f2, &f1) == DIR_WINS);       // a directory replaced the file here

        uint32_t mode;
        int up;
        CHECK(!dir_mode(&d1, &d1, &d1, &mode, &up));
        CHECK(dir_mode(&d2, &d1, &d1, &mode, &up) && up && mode == 0700);  // changed here
        CHECK(dir_mode(&d1, &d2, &d1, &mode, &up) && !up && mode == 0700); // changed there
        CHECK(dir_mode(&d1, &d2, &none, &mode, &up) && up && mode == 0755); // new: here wins
}

int
main(void)
{
        test_paths();
        test_ignore();
        test_plan();
        if (failed) return 1;
        printf("unit: ok\n");
        return 0;
}
