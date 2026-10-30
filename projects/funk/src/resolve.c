/* resolve.c -- closure conversion shared by the interpreter and cgen.

   Every lambda owns a flat frame: params in slots 0..n-1, then one slot
   per let/where/pattern variable in its body. A variable from an enclosing
   lambda becomes a free variable, copied into the closure when it is
   created; intermediate lambdas capture it too. Frames therefore never
   outlive their call, so both backends keep them on the C stack.

   A non-recursive local binding becomes a thunk: its body is a
   zero-parameter lambda, and references to it force the thunk. Right sides
   that are safe to evaluate early are evaluated where they are bound
   instead (`eager`): the difference cannot be observed, and the thunk
   costs an allocation per binding. */
#include "funk.h"

typedef struct { void *owner; int slot; FreeSrc src; } Cap;
typedef struct Fn { Lambda *lam; struct Fn *parent; int nslots; VEC(Cap) free; } Fn;
typedef struct Scope {
  const char *name;
  Fn *fn;
  int slot;
  Lambda *known;
  bool lazy;
  struct Scope *next;
} Scope;

/* resolve.c's state, one per session (embedding.md, phase 2) */
typedef struct {
  int lam_counter;
  VEC(struct { Bind *b; char v; }) eager_fn_memo; /* v: 1 checking, 2 eager, 3 not */
} ResolveState;
static const ResolveState resolve_init = {0};
#define ST ((ResolveState *)fk_here()->cc[CC_RESOLVE])

void *resolve_state_new(void) {
  ResolveState *s = malloc(sizeof *s);
  if (!s) fk_fatal("out of memory");
  *s = resolve_init;
  return s;
}


static Scope *push(Scope *sc, const char *name, Fn *fn, int slot, Lambda *known, bool lazy) {
  Scope *s = NEW(Scope);
  *s = (Scope){name, fn, slot, known, lazy, sc};
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

/* Primitives that are total, have no effect and take constant time. The
   division family is total only with a literal, non-zero divisor. */
static bool total_prim(const char *c) {
  static const char *const names[] = {"addi", "subi", "muli", "negi", "absi", "addf", "subf", "mulf", "fdiv", "negf",
                                      "absf", "sqrt", "exp",  "log",  "sin",  "cos",  "pow",  "tofloat", "ord",
                                      "eqw",  "new",  "ltw",  "lew",  "gtw",  "gew",  "eqf",  "nef",  "ltf",
                                      "lef",  "gtf",  "gef",  "band", "bor",  "bxor", "bnot"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    if (strcmp(names[i], c) == 0) return true;
  return false;
}

static bool nonzero_literal(Expr *e) { return (e->k == E_INT && e->ival) || (e->k == E_FLOAT && e->fval); }

static bool eager(Expr *e, Scope *sc);

/* A full call of global g is eager when g's body is, under its parameters,
   which are values: `max` at Int. Recursion, direct or not, might not
   terminate, so a function being checked counts as not eager. Keyed by the
   Bind, since a failed REPL chunk frees its global indices for reuse. */
static bool eager_fn(int g) {
  Bind *b = CS->globals.a[g].bind;
  if (!b || b->e->k != E_LAM) return false;
  while (ST->eager_fn_memo.n <= g) VPUSH(ST->eager_fn_memo, ((__typeof__(*ST->eager_fn_memo.a)){NULL, 0}));
  if (ST->eager_fn_memo.a[g].b == b && ST->eager_fn_memo.a[g].v) return ST->eager_fn_memo.a[g].v == 2;
  ST->eager_fn_memo.a[g].b = b;
  ST->eager_fn_memo.a[g].v = 1;
  Lambda *L = b->e->lam;
  Scope *sc = NULL; /* parameters are values; a call of one is unknown */
  for (int i = 0; i < L->n; i++) sc = push(sc, L->params[i], NULL, i, NULL, false);
  bool ok = eager(L->body, sc);
  ST->eager_fn_memo.a[g].v = ok ? 2 : 3;
  return ok;
}

/* Can e be evaluated now instead of at its first use, with no way to tell?
   It must terminate, cannot fail and has no effect: literals, constructors,
   lambdas, variables already evaluated, total primitives, full calls of
   global functions whose bodies are such expressions (eager_fn), and `if`
   over them. */
static bool eager(Expr *e, Scope *sc) {
  switch (e->k) {
  case E_LAM: case E_CON: case E_INT: case E_FLOAT: case E_CHAR: case E_STR: return true;
  case E_VAR:
    for (Scope *s = sc; s; s = s->next)
      if (s->name == e->name) return !s->lazy; /* forcing a thunk may fail */
    return true; /* a global: evaluated before anything that uses it */
  case E_ANN: return eager(e->body, sc);
  case E_IF: return eager(e->cond, sc) && eager(e->then_, sc) && eager(e->else_, sc);
  case E_APP: {
    for (int i = 0; i < e->n; i++)
      if (!eager(e->args[i], sc)) return false;
    if (e->fn->k == E_CON) return e->n <= fk_cons[e->fn->idx].arity;
    if (e->fn->k != E_VAR) return false;
    for (Scope *s = sc; s; s = s->next)
      if (s->name == e->fn->name) return false; /* a local function */
    int g = global_lookup(e->fn->name);
    int p = g >= 0 ? CS->globals.a[g].prim : -1;
    if (p < 0) return g >= 0 && CS->globals.a[g].bind && CS->globals.a[g].bind->e->k == E_LAM &&
                      e->n == CS->globals.a[g].bind->e->lam->n && eager_fn(g);
    if (e->n != fk_prims[p].arity) return false;
    const char *c = prim_cname(p);
    if (total_prim(c)) return true;
    if (!strcmp(c, "shl") || !strcmp(c, "shr")) return e->args[1]->k == E_INT && e->args[1]->ival >= 0; /* a negative shift fails */
    return (!strcmp(c, "div") || !strcmp(c, "mod") || !strcmp(c, "quot") || !strcmp(c, "rem")) &&
           nonzero_literal(e->args[1]);
  }
  default: return false;
  }
}

static Scope *group(Group *g, Fn *f, Scope *sc) {
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    b->slot = f->nslots++;
    if (!g->rec && !eager(b->e, sc) && !b->acts) { /* an effectful binding runs where it is bound */
      Lambda *L = NEW(Lambda);
      L->body = b->e;
      L->line = b->line;
      L->col = b->col;
      L->name = b->name;
      b->thunk = L;
    }
    sc = push(sc, b->name, f, b->slot, b->e->k == E_LAM ? b->e->lam : NULL, b->thunk != NULL);
  }
  for (int i = 0; i < g->n; i++) {
    if (g->b[i]->thunk) lambda(g->b[i]->thunk, f, sc);
    else expr(g->b[i]->e, f, sc);
  }
  return sc;
}

static Scope *pat(Pat *p, Fn *f, Scope *sc) {
  if (p->k == P_VAR) {
    p->slot = f->nslots++;
    return push(sc, p->name, f, p->slot, NULL, false);
  }
  for (int i = 0; i < p->n; i++) sc = pat(p->args[i], f, sc);
  return sc;
}

static void expr(Expr *e, Fn *f, Scope *sc) {
  switch (e->k) {
  case E_VAR: {
    for (Scope *s = e->global_ref ? NULL : sc; s; s = s->next)
      if (s->name == e->name) {
        FreeSrc src = capture(f, s->fn, s->slot);
        e->vk = src.vk;
        e->idx = src.idx;
        e->known = s->known;
        e->lazy = s->lazy;
        return;
      }
    e->vk = VK_GLOBAL;
    e->idx = global_lookup(e->name);
    e->known = CS->globals.a[e->idx].lam;
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
  L->id = ST->lam_counter++;
  VPUSH(CS->all_lambdas, L);
  for (int i = 0; i < L->n; i++) sc = push(sc, L->params[i], &fn, i, NULL, false);
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
