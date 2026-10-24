/* resolve.c -- closure conversion shared by the interpreter and cgen.

   Every lambda owns a flat frame: params in slots 0..n-1, then one slot
   per let/where/pattern variable in its body. A variable from an enclosing
   lambda becomes a free variable, copied into the closure when it is
   created; intermediate lambdas capture it too. Frames therefore never
   outlive their call, so both backends keep them on the C stack. */
#include "funk.h"

typedef struct { void *owner; int slot; FreeSrc src; } Cap;
typedef struct Fn { Lambda *lam; struct Fn *parent; int nslots; VEC(Cap) free; } Fn;
typedef struct Scope {
  const char *name;
  Fn *fn;
  int slot;
  Lambda *known;
  struct Scope *next;
} Scope;

static int lam_counter;

static Scope *push(Scope *sc, const char *name, Fn *fn, int slot, Lambda *known) {
  Scope *s = NEW(Scope);
  *s = (Scope){name, fn, slot, known, sc};
  return s;
}

static FreeSrc capture(Fn *f, Fn *owner, int slot) {
  if (f == owner) return (FreeSrc){VK_LOCAL, slot};
  for (int i = 0; i < f->free.n; i++)
    if (f->free.a[i].owner == owner && f->free.a[i].slot == slot) return (FreeSrc){VK_FREE, i};
  FreeSrc src = capture(f->parent, owner, slot);
  VPUSH(f->free, ((Cap){owner, slot, src}));
  return (FreeSrc){VK_FREE, f->free.n - 1};
}

static void expr(Expr *e, Fn *f, Scope *sc);
static void lambda(Lambda *L, Fn *parent, Scope *sc);

static Scope *group(Group *g, Fn *f, Scope *sc) {
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    b->slot = f->nslots++;
    sc = push(sc, b->name, f, b->slot, b->e->k == E_LAM ? b->e->lam : NULL);
  }
  for (int i = 0; i < g->n; i++) expr(g->b[i]->e, f, sc);
  return sc;
}

static Scope *pat(Pat *p, Fn *f, Scope *sc) {
  if (p->k == P_VAR) {
    p->slot = f->nslots++;
    return push(sc, p->name, f, p->slot, NULL);
  }
  for (int i = 0; i < p->n; i++) sc = pat(p->args[i], f, sc);
  return sc;
}

static void expr(Expr *e, Fn *f, Scope *sc) {
  switch (e->k) {
  case E_VAR: {
    for (Scope *s = sc; s; s = s->next)
      if (s->name == e->name) {
        FreeSrc src = capture(f, s->fn, s->slot);
        e->vk = src.vk;
        e->idx = src.idx;
        e->known = s->known;
        return;
      }
    e->vk = VK_GLOBAL;
    e->idx = global_lookup(e->name);
    e->known = globals.a[e->idx].lam;
    return;
  }
  case E_INT: {
    Type *t = repr(e->ty);
    if (t->k == TY_CON && strcmp(t->name, "Float") == 0) {
      e->k = E_FLOAT;
      e->fval = (double)e->ival;
    }
    return;
  }
  case E_APP:
    expr(e->fn, f, sc);
    for (int i = 0; i < e->n; i++) expr(e->args[i], f, sc);
    return;
  case E_LAM: lambda(e->lam, f, sc); return;
  case E_LET: expr(e->body, f, group(&e->g, f, sc)); return;
  case E_IF:
    expr(e->cond, f, sc);
    expr(e->then_, f, sc);
    expr(e->else_, f, sc);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) expr(e->scruts[i], f, sc);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Scope *s = sc;
      for (int i = 0; i < alt->np; i++) s = pat(alt->pats[i], f, s);
      for (int i = 0; i < alt->rhs.nwg; i++) s = group(&alt->rhs.wg[i], f, s);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) expr(alt->rhs.guards[i], f, s);
        expr(alt->rhs.bodies[i], f, s);
      }
    }
    return;
  case E_ANN: expr(e->body, f, sc); return;
  default: return;
  }
}

static void lambda(Lambda *L, Fn *parent, Scope *sc) {
  Fn fn = {L, parent, L->n, {0}};
  L->id = lam_counter++;
  VPUSH(all_lambdas, L);
  for (int i = 0; i < L->n; i++) sc = push(sc, L->params[i], &fn, i, NULL);
  expr(L->body, &fn, sc);
  L->nslots = fn.nslots;
  L->nfree = fn.free.n;
  L->free = xmalloc((size_t)fn.free.n * sizeof *L->free + 1);
  for (int i = 0; i < fn.free.n; i++) L->free[i] = fn.free.a[i].src;
  free(fn.free.a);
}

void resolve_top(Bind *b) {
  if (b->e->k == E_LAM) {
    lambda(b->e->lam, NULL, NULL);
    return;
  }
  Lambda *L = NEW(Lambda);
  L->body = b->e;
  L->line = b->line;
  L->col = b->col;
  L->name = b->name;
  b->init = L;
  lambda(L, NULL, NULL);
}
