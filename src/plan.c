#define _DEFAULT_SOURCE

#include <stdlib.h>
#include <string.h>

#include "plan.h"

State
state_copy(const State *st)
{
        State c = *st;
        if (c.link) c.link = strdup(c.link);
        return c;
}

void
state_free(State *st)
{
        free(st->link);
        st->link = NULL;
}

int
same(const State *a, const State *b)
{
        if (a->type != b->type) return 0;
        switch (a->type) {
        case 'f': return a->size == b->size && a->mtime == b->mtime && a->mode == b->mode;
        case 'd': return a->mode == b->mode;
        /* A link with no target read (the agent doesn't send it) is never
         * the same as another: whoever has it asks the remote instead */
        case 'l': return a->link && b->link && !strcmp(a->link, b->link);
        }
        return 1;
}

int
same_local(const State *l, const State *r)
{
        return same(l, r) && (l->type != 'f' || l->stamp == r->stamp);
}

int
same_remote(const State *m, const State *r)
{
        return same(m, r) && !(m->type == 'f' && m->written);
}

int
same_data(const State *a, const State *b)
{
        return a->type == 'f' && b->type == 'f' && a->size == b->size && a->mtime == b->mtime;
}

void
plan_add(Plan *plan, ActType type, int up, const char *rel, const State *L, const State *M, const State *st)
{
        static const State none = { 0 };
        Da_append(plan, (Action) {
                                .type = type,
                                .up   = up,
                                .rel  = strdup(rel),
                                .L    = state_copy(L ? L : &none),
                                .M    = state_copy(M ? M : &none),
                                .st   = state_copy(st ? st : &none),
                                });
}

void
plan_free(Plan *plan)
{
        Da_foreach(a, *plan)
        {
                free(a->rel);
                state_free(&a->L);
                state_free(&a->M);
                state_free(&a->st);
        }
        Da_destroy(plan);
}

void
plan_file(Plan *plan, const char *rel, const State *L, const State *M, const State *R, int what)
{
        int lc = !same_local(L, R), rc = !same_remote(M, R);
        if (!lc && !rc) return; // nothing: also what our own changes look like
        if (lc && rc && same(L, M)) {
                /* Both changed the same way (or first sync) */
                plan_add(plan, ACT_RECORD, 0, rel, L, M, L);
                return;
        }

        int up, conflict = 0;
        if (!rc)
                up = 1;
        else if (!lc)
                up = 0;
        else if (L->type == 0 || M->type == 0)
                up = L->type != 0; // an edit beats a deletion
        else {
                up       = L->mtime >= M->mtime;
                conflict = !same_data(L, M);
        }

        const State *W = up ? L : M; // what wins
        if (W->type == 0) {
                plan_add(plan, ACT_REMOVE, up, rel, L, M, NULL);
        } else if (!conflict && L->type == 'f' && M->type == 'f' &&
                   (up ? what == SYNC_ATTR && L->size == M->size : same_data(M, L) && M->mode != L->mode)) {
                /* Only the mode (and up, the times) changed. (Down with the
                 * same mode, only a write there said it changed: the content.) */
                plan_add(plan, ACT_ATTRS, up, rel, L, M, NULL);
        } else {
                plan_add(plan, ACT_COPY, up, rel, L, M, NULL);
                plan->items[plan->count - 1].conflict = conflict;
        }
}

DirCase
decide_dir(const State *L, const State *M, const State *R)
{
        int ld = L->type == 'd', md = M->type == 'd';
        if (ld && md) return DIR_BOTH;
        const State *D = ld ? L : M; // the directory
        const State *O = ld ? M : L; // what the other side has instead
        if (O->type == 0) return R->type == 'd' ? DIR_GONE : DIR_WINS;
        int other_changed = !same(O, R);
        int dir_changed   = R->type != 'd' || R->mode != D->mode;
        return other_changed && !dir_changed ? DIR_LOST : DIR_WINS;
}

int
dir_mode(const State *L, const State *M, const State *R, uint32_t *mode, int *up)
{
        if (L->mode == M->mode) return 0;
        *up   = R->type != 'd' || R->mode != L->mode; // new, or changed here
        *mode = *up ? L->mode : M->mode;
        return 1;
}
