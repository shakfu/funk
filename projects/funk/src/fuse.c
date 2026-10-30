/* fuse.c -- fusion of list pipelines into one loop (docs/dev/fusion.md).

   A chain of saturated calls to prelude list functions -- an optional
   consumer, transformers, a producer -- becomes a local recursive function
   over the stages' state. lift_locals, ho_specialize, the typed backend
   and TRMC then compile it like a hand-written loop. A chain is fused only
   if no stage function can perform an effect, at most one can fail, and
   none before a short-circuiting stage can fail: the result, the error and
   termination are then those of the original. Runs after lift_locals, so
   local helpers are globals the analyses can see. */
#include "funk.h"

typedef struct Nm { const char *name; struct Nm *next; } Nm;

static Nm *nm(Nm *s, const char *name) {
  Nm *n = NEW(Nm);
  *n = (Nm){name, s};
  return n;
}

static bool in(Nm *s, const char *name) {
  for (; s; s = s->next)
    if (s->name == name) return true;
  return false;
}

static Nm *pat_nm(Pat *p, Nm *s) {
  if (p->k == P_VAR) return nm(s, p->name);
  for (int i = 0; i < p->n; i++) s = pat_nm(p->args[i], s);
  return s;
}

static Nm *group_nm(Group *g, Nm *s) {
  for (int i = 0; i < g->n; i++) s = nm(s, g->b[i]->name);
  return s;
}

/* The global e names, unless a local shadows it; -1 otherwise. */
static int gref(Expr *e, Nm *loc) {
  if (e->k != E_VAR || (!e->global_ref && in(loc, e->name))) return -1;
  return global_lookup(e->name);
}

static Bind *gbind(int g) {
  Bind *b = g >= 0 ? CS->globals.a[g].bind : NULL;
  if (b && b->own_of) b = b->own_of; /* a reuse copy computes what it copies: fusion sees concat, not concat@reuse0 */
  return b && b->e->k == E_LAM && b->e->lam->ptypes ? b : NULL;
}

/* ---------------------------------------------- effects and failure */

static bool impure_prim(int g) { return type_acts(CS->globals.a[g].type); }

static bool divides(const char *n) {
  return !strcmp(n, "div") || !strcmp(n, "mod") || !strcmp(n, "quot") || !strcmp(n, "rem");
}

static bool shifts(const char *n) { return !strcmp(n, "shiftL") || !strcmp(n, "shiftR"); } /* fail on a negative count */

static bool partial_prim(const char *n) {
  static const char *const p[] = {"error", "chr", "floor", "ceiling", "round", "truncate", "readInt", "readFloat", "getLine",
                                  "primNewArray", "primNewRing", "mallocBytes", "newCString", "peekCString", "deRefStablePtr",
                                  "freeStablePtr", "waitEvent", "pollEvent", "indexString",
                                  "writeArrayString", "putArray", "getArray", "copyArray"};
  for (size_t i = 0; i < sizeof p / sizeof p[0]; i++)
    if (!strcmp(n, p[i])) return true;
  return divides(n) || shifts(n);
}

bool prim_call_total(Expr *e) {
  const char *n = CS->globals.a[e->fn->idx].name;
  if (divides(n)) return e->n == 2 && e->args[1]->k == E_INT && e->args[1]->ival != 0;
  if (shifts(n)) return e->n == 2 && e->args[1]->k == E_INT && e->args[1]->ival >= 0;
  return !partial_prim(n);
}

/* May the global g, used as a function value, be called? tot: without
   failing; otherwise: without effects. */
static bool gval_ok(int g, bool tot) {
  if (g < 0) return false;
  if (CS->globals.a[g].prim >= 0) return tot ? !partial_prim(CS->globals.a[g].name) : !impure_prim(g);
  if (CS->globals.a[g].ffi) return !tot && CS->globals.a[g].ffi->pure; /* C may fail in ways funk cannot see */
  Bind *b = CS->globals.a[g].bind;
  if (!b) return !CS->globals.a[g].lam; /* a value, evaluated at startup, or an unanalyzed function */
  if (b->e->k != E_LAM) return true;
  return tot ? b->total : b->pure;
}

/* Is e free of effects (tot: free of failure)? Calls are allowed to
   globals that are, to names in `call` (parameters of the function being
   analyzed, assumed to be bound to such functions, and local lambdas),
   and to lambdas. */
static bool ok_e(Expr *e, Nm *loc, Nm *call, bool tot);

static bool ok_group(Group *g, Nm **loc, Nm **call, bool tot) {
  bool lams = true;
  for (int i = 0; i < g->n; i++) lams &= g->b[i]->e->k == E_LAM;
  if (tot && g->rec && !lams) return false; /* a recursive value */
  *loc = group_nm(g, *loc);
  for (int i = 0; i < g->n; i++)
    if (g->b[i]->e->k == E_LAM && !(tot && g->rec)) *call = nm(*call, g->b[i]->name);
  for (int i = 0; i < g->n; i++)
    if (!ok_e(g->b[i]->e, *loc, *call, tot)) return false;
  return true;
}

static bool ok_e(Expr *e, Nm *loc, Nm *call, bool tot) {
  switch (e->k) {
  case E_VAR: {
    int g = gref(e, loc);
    return g < 0 || gval_ok(g, tot);
  }
  case E_APP: {
    for (int i = 0; i < e->n; i++)
      if (!ok_e(e->args[i], loc, call, tot)) return false;
    Expr *f = e->fn;
    if (f->k == E_CON) return true;
    if (f->k == E_LAM) return ok_e(f, loc, call, tot);
    if (f->k != E_VAR) return false;
    int g = gref(f, loc);
    if (g < 0) return in(loc, f->name) && in(call, f->name);
    if (CS->globals.a[g].prim >= 0) {
      const char *n = CS->globals.a[g].name;
      if (e->n > fk_prims[CS->globals.a[g].prim].arity) return false; /* applies an unknown result */
      if (!tot) return !impure_prim(g);
      if (divides(n)) return e->n == 2 && e->args[1]->k == E_INT && e->args[1]->ival != 0;
      if (shifts(n)) return e->n == 2 && e->args[1]->k == E_INT && e->args[1]->ival >= 0;
      return !partial_prim(n);
    }
    if (CS->globals.a[g].ffi) return e->n <= CS->globals.a[g].ffi->arity && gval_ok(g, tot);
    Bind *b = gbind(g);
    if (!b || e->n > b->e->lam->n) return false;
    return tot ? b->total : b->pure;
  }
  case E_LAM: {
    for (int i = 0; i < e->lam->n; i++) loc = nm(loc, e->lam->params[i]);
    return ok_e(e->lam->body, loc, call, tot);
  }
  case E_LET: return ok_group(&e->g, &loc, &call, tot) && ok_e(e->body, loc, call, tot);
  case E_IF: return ok_e(e->cond, loc, call, tot) && ok_e(e->then_, loc, call, tot) && ok_e(e->else_, loc, call, tot);
  case E_CASE:
    if (tot && !case_covers(e, !in(loc, intern("otherwise")))) return false;
    for (int i = 0; i < e->ns; i++)
      if (!ok_e(e->scruts[i], loc, call, tot)) return false;
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = loc, *c = call;
      for (int i = 0; i < alt->np; i++) {
        Expr *s = e->scruts[i];
        t = pat_nm(alt->pats[i], t);
        if (alt->pats[i]->k == P_VAR && s->k == E_VAR && !s->global_ref && in(call, s->name))
          c = nm(c, alt->pats[i]->name); /* `case f of g -> g x`, as clauses desugar */
      }
      for (int i = 0; i < alt->rhs.nwg; i++)
        if (!ok_group(&alt->rhs.wg[i], &t, &c, tot)) return false;
      for (int i = 0; i < alt->rhs.ng; i++)
        if ((alt->rhs.guards[i] && !ok_e(alt->rhs.guards[i], t, c, tot)) || !ok_e(alt->rhs.bodies[i], t, c, tot))
          return false;
    }
    return true;
  case E_ANN: return ok_e(e->body, loc, call, tot);
  default: return true;
  }
}

/* May the function value f be called per element? Values only: a global,
   a lambda, or a partial application of a global to such arguments. */
static bool fn_ok(Expr *f, Nm *loc, bool tot) {
  switch (f->k) {
  case E_VAR: return gval_ok(gref(f, loc), tot);
  case E_LAM: return ok_e(f, loc, NULL, tot);
  case E_APP: {
    for (int i = 0; i < f->n; i++)
      if (!ok_e(f->args[i], loc, NULL, tot)) return false;
    int g = gref(f->fn, loc);
    if (g >= 0 && CS->globals.a[g].prim >= 0) return f->n < fk_prims[CS->globals.a[g].prim].arity && gval_ok(g, tot);
    Bind *b = gbind(g);
    return b && f->n < b->e->lam->n && (tot ? b->total : b->pure);
  }
  default: return false;
  }
}

static void analyze(Bind **bs, int n) {
  for (int i = 0; i < n; i++) {
    bs[i]->pure = true;
    bs[i]->total = false;
  }
  for (int tot = 0; tot < 2; tot++)
    for (bool changed = true; changed;) {
      changed = false;
      for (int i = 0; i < n; i++) {
        Bind *b = bs[i];
        if (tot ? b->total || !b->pure : !b->pure) continue;
        Lambda *L = b->e->lam;
        Nm *loc = NULL;
        for (int j = 0; j < L->n; j++) loc = nm(loc, L->params[j]);
        bool ok = ok_e(L->body, loc, loc, tot);
        if (tot ? ok : !ok) {
          if (tot) b->total = true;
          else b->pure = false;
          changed = true;
        }
      }
    }
}

/* ------------------------------------------------------ expressions */

enum { F_NONE, F_MAP, F_FILTER, F_TAKE, F_TAKEWHILE, F_CONCAT, F_FOLDL, F_ANY, F_ALL, F_RANGE, F_REPLICATE, F_N };

/* fuse.c's state, one per session (embedding.md, phase 2) */
typedef struct {
  int fline, fcol, nfresh;
  Bind *core[F_N];
  Nm *floc, *flazy;
  bool changed;
} FuseState;
static const FuseState fuse_init = {0};
#define ST ((FuseState *)fk_here()->cc[CC_FUSE])

void *fuse_state_new(void) {
  FuseState *s = malloc(sizeof *s);
  if (!s) fk_fatal("out of memory");
  *s = fuse_init;
  return s;
}


static Expr *mk(int k, Type *t) {
  Expr *e = NEW(Expr);
  e->k = k;
  e->line = ST->fline;
  e->col = ST->fcol;
  e->ty = t;
  return e;
}

static const char *fresh(void) { return intern(xsprintf("$f%d", ST->nfresh++)); }

static Type *t_int(void) { return tcon_new("Int", 0, NULL); }
static Type *t_bool(void) { return tcon_new("Bool", 0, NULL); }
static Type *t_fun(Type *a, Type *b) { return tcon_new("->", 2, (Type *[]){a, b}); }

static Type *elem_of(Type *t) {
  t = t ? repr(t) : NULL;
  return t && t->k == TY_CON && !strcmp(t->name, "[]") && t->n == 1 ? t->args[0] : NULL;
}

static Expr *evar(const char *name, Type *t) {
  Expr *e = mk(E_VAR, t);
  e->name = name;
  return e;
}

static Expr *eglobal(const char *name, Type *t) {
  Expr *e = evar(intern(name), t);
  e->global_ref = true;
  return e;
}

static Expr *eint(long long v) {
  Expr *e = mk(E_INT, t_int());
  e->ival = v;
  return e;
}

static Expr *econ(int con, const char *name, Type *t) {
  Expr *e = mk(E_CON, t);
  e->idx = con;
  e->name = intern(name);
  return e;
}

static Expr *ebool(bool b) { return econ(b ? FK_C_TRUE : FK_C_FALSE, b ? "True" : "False", t_bool()); }

static Expr *eapp(Expr *f, int n, Expr **args, Type *t) {
  Expr *e = mk(E_APP, t);
  e->fn = f;
  e->n = n;
  e->args = xmalloc((size_t)n * sizeof *e->args + 1);
  copy_n(e->args, args, (size_t)n * sizeof *args);
  return e;
}

static Expr *eapp1(Expr *f, Expr *a, Type *t) { return eapp(f, 1, &a, t); }

/* An Int primitive applied to two Int operands. */
static Expr *eprim(const char *name, Expr *a, Expr *b, Type *res) {
  return eapp(eglobal(name, t_fun(t_int(), t_fun(t_int(), res))), 2, (Expr *[]){a, b}, res);
}

static Expr *eif(Expr *c, Expr *t, Expr *f, Type *ty) {
  Expr *e = mk(E_IF, ty);
  e->cond = c;
  e->then_ = t;
  e->else_ = f;
  return e;
}

static Pat *pvar(const char *name, Type *t) {
  Pat *p = NEW(Pat);
  *p = (Pat){.k = P_VAR, .line = ST->fline, .col = ST->fcol, .name = name, .ty = t};
  return p;
}

static Alt *alt1(Pat *p, Expr *body) {
  Alt *a = NEW(Alt);
  a->np = 1;
  a->pats = xmalloc(sizeof *a->pats);
  a->pats[0] = p;
  a->rhs.ng = 1;
  a->rhs.guards = xcalloc(1, sizeof(Expr *));
  a->rhs.bodies = xmalloc(sizeof(Expr *));
  a->rhs.bodies[0] = body;
  return a;
}

static Expr *ecase(Expr *s, int na, Alt **alts, Type *t) {
  Expr *e = mk(E_CASE, t);
  e->name = CS->cur_file;
  e->ns = 1;
  e->scruts = xmalloc(sizeof *e->scruts);
  e->scruts[0] = s;
  e->na = na;
  e->alts = xmalloc((size_t)na * sizeof *e->alts);
  copy_n(e->alts, alts, (size_t)na * sizeof *alts);
  return e;
}

/* case s of x -> body: s evaluated here, once. */
static Expr *ebind(Expr *s, const char *x, Expr *body) {
  return ecase(s, 1, (Alt *[]){alt1(pvar(x, s->ty), body)}, body->ty);
}

/* case l of [] -> nil; (x : rest) -> cons */
static Expr *ematch(Expr *l, Expr *nil, const char *x, const char *rest, Expr *cons) {
  Type *lt = l->ty, *et = elem_of(lt);
  Pat *pn = NEW(Pat), *pc = NEW(Pat);
  *pn = (Pat){.k = P_CON, .line = ST->fline, .col = ST->fcol, .name = intern("[]"), .con = FK_C_NIL, .ty = lt};
  *pc = (Pat){.k = P_CON, .line = ST->fline, .col = ST->fcol, .name = intern(":"), .con = FK_C_CONS, .n = 2, .ty = lt};
  pc->args = xmalloc(2 * sizeof *pc->args);
  pc->args[0] = pvar(x, et);
  pc->args[1] = pvar(rest, lt);
  return ecase(l, 2, (Alt *[]){alt1(pn, nil), alt1(pc, cons)}, nil->ty);
}

static Expr *dummy(Type *t) {
  t = t ? repr(t) : NULL;
  if (!t || t->k != TY_CON) return NULL;
  if (!strcmp(t->name, "Int") && !t->n) return eint(0);
  if (!strcmp(t->name, "Bool")) return ebool(false);
  if (!strcmp(t->name, "[]")) return econ(FK_C_NIL, "[]", t);
  if (!strcmp(t->name, "()")) return econ(FK_C_UNIT, "()", t);
  return NULL;
}

/* Copy of e with x replaced by a copy of arg, respecting shadowing. */
static Expr *subst(Expr *e, const char *x, Expr *arg);

static void subst_rhs(Rhs *r, const char *x, Expr *arg) {
  for (int i = 0; i < r->nwg; i++)
    for (int j = 0; j < r->wg[i].n; j++)
      if (r->wg[i].b[j]->name == x) return;
  for (int i = 0; i < r->nwg; i++)
    for (int j = 0; j < r->wg[i].n; j++) r->wg[i].b[j]->e = subst(r->wg[i].b[j]->e, x, arg);
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) r->guards[i] = subst(r->guards[i], x, arg);
    r->bodies[i] = subst(r->bodies[i], x, arg);
  }
}

static Expr *subst_in(Expr *e, const char *x, Expr *arg) { /* e is already a copy */
  switch (e->k) {
  case E_VAR:
    if (e->name == x && !e->global_ref) {
      Type *t = e->ty;
      *e = *copy_expr(arg);
      if (!e->ty) e->ty = t;
    }
    return e;
  case E_APP:
    subst_in(e->fn, x, arg);
    for (int i = 0; i < e->n; i++) subst_in(e->args[i], x, arg);
    return e;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++)
      if (e->lam->params[i] == x) return e;
    subst_in(e->lam->body, x, arg);
    return e;
  case E_LET:
    for (int i = 0; i < e->g.n; i++)
      if (e->g.b[i]->name == x) return e;
    for (int i = 0; i < e->g.n; i++) subst_in(e->g.b[i]->e, x, arg);
    subst_in(e->body, x, arg);
    return e;
  case E_IF:
    subst_in(e->cond, x, arg);
    subst_in(e->then_, x, arg);
    subst_in(e->else_, x, arg);
    return e;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) subst_in(e->scruts[i], x, arg);
    for (int a = 0; a < e->na; a++) {
      bool shadow = false;
      for (int i = 0; i < e->alts[a]->np; i++) shadow |= in(pat_nm(e->alts[a]->pats[i], NULL), x);
      if (!shadow) subst_rhs(&e->alts[a]->rhs, x, arg);
    }
    return e;
  case E_ANN: subst_in(e->body, x, arg); return e;
  default: return e;
  }
}

static Expr *subst(Expr *e, const char *x, Expr *arg) { return subst_in(copy_expr(e), x, arg); }

/* Names bound anywhere in e, and local variables used in e. */
static Nm *binders(Expr *e, Nm *s);

static Nm *binders_rhs(Rhs *r, Nm *s) {
  for (int i = 0; i < r->nwg; i++) {
    s = group_nm(&r->wg[i], s);
    for (int j = 0; j < r->wg[i].n; j++) s = binders(r->wg[i].b[j]->e, s);
  }
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) s = binders(r->guards[i], s);
    s = binders(r->bodies[i], s);
  }
  return s;
}

static Nm *binders(Expr *e, Nm *s) {
  switch (e->k) {
  case E_APP:
    s = binders(e->fn, s);
    for (int i = 0; i < e->n; i++) s = binders(e->args[i], s);
    return s;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) s = nm(s, e->lam->params[i]);
    return binders(e->lam->body, s);
  case E_LET:
    s = group_nm(&e->g, s);
    for (int i = 0; i < e->g.n; i++) s = binders(e->g.b[i]->e, s);
    return binders(e->body, s);
  case E_IF: return binders(e->else_, binders(e->then_, binders(e->cond, s)));
  case E_CASE:
    for (int i = 0; i < e->ns; i++) s = binders(e->scruts[i], s);
    for (int a = 0; a < e->na; a++) {
      for (int i = 0; i < e->alts[a]->np; i++) s = pat_nm(e->alts[a]->pats[i], s);
      s = binders_rhs(&e->alts[a]->rhs, s);
    }
    return s;
  case E_ANN: return binders(e->body, s);
  default: return s;
  }
}

static bool uses_any(Expr *e, Nm *names);

static bool uses_rhs(Rhs *r, Nm *names) {
  for (int i = 0; i < r->nwg; i++)
    for (int j = 0; j < r->wg[i].n; j++)
      if (uses_any(r->wg[i].b[j]->e, names)) return true;
  for (int i = 0; i < r->ng; i++)
    if ((r->guards[i] && uses_any(r->guards[i], names)) || uses_any(r->bodies[i], names)) return true;
  return false;
}

static bool uses_any(Expr *e, Nm *names) {
  switch (e->k) {
  case E_VAR: return !e->global_ref && in(names, e->name);
  case E_APP:
    if (uses_any(e->fn, names)) return true;
    for (int i = 0; i < e->n; i++)
      if (uses_any(e->args[i], names)) return true;
    return false;
  case E_LAM: return uses_any(e->lam->body, names);
  case E_LET:
    for (int i = 0; i < e->g.n; i++)
      if (uses_any(e->g.b[i]->e, names)) return true;
    return uses_any(e->body, names);
  case E_IF: return uses_any(e->cond, names) || uses_any(e->then_, names) || uses_any(e->else_, names);
  case E_CASE:
    for (int i = 0; i < e->ns; i++)
      if (uses_any(e->scruts[i], names)) return true;
    for (int a = 0; a < e->na; a++)
      if (uses_rhs(&e->alts[a]->rhs, names)) return true;
    return false;
  case E_ANN: return uses_any(e->body, names);
  default: return false;
  }
}

static int occ(Expr *e, const char *x) {
  Nm one = {x, NULL};
  if (e->k == E_VAR) return uses_any(e, &one);
  int n = 0;
  if (e->k == E_APP) {
    n = occ(e->fn, x);
    for (int i = 0; i < e->n; i++) n += occ(e->args[i], x);
    return n;
  }
  return uses_any(e, &one) ? 2 : 0; /* only direct arguments count as one use */
}

/* Mark the globals of a body copied out of its function. */
static void pin(Expr *e, Nm *inner) {
  switch (e->k) {
  case E_VAR: if (!in(inner, e->name)) e->global_ref = true; return;
  case E_APP:
    pin(e->fn, inner);
    for (int i = 0; i < e->n; i++) pin(e->args[i], inner);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) inner = nm(inner, e->lam->params[i]);
    pin(e->lam->body, inner);
    return;
  case E_IF:
    pin(e->cond, inner);
    pin(e->then_, inner);
    pin(e->else_, inner);
    return;
  case E_ANN: pin(e->body, inner); return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) pin(e->scruts[i], inner);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = inner;
      for (int i = 0; i < alt->np; i++) t = pat_nm(alt->pats[i], t);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) pin(alt->rhs.guards[i], t);
        pin(alt->rhs.bodies[i], t);
      }
    }
    return;
  default: return;
  }
}

static bool has_let(Expr *e) {
  switch (e->k) {
  case E_LET: return true;
  case E_APP:
    if (has_let(e->fn)) return true;
    for (int i = 0; i < e->n; i++)
      if (has_let(e->args[i])) return true;
    return false;
  case E_LAM: return has_let(e->lam->body);
  case E_IF: return has_let(e->cond) || has_let(e->then_) || has_let(e->else_);
  case E_CASE:
    for (int a = 0; a < e->na; a++)
      if (e->alts[a]->rhs.nwg) return true;
    for (int i = 0; i < e->ns; i++)
      if (has_let(e->scruts[i])) return true;
    for (int a = 0; a < e->na; a++)
      for (int i = 0; i < e->alts[a]->rhs.ng; i++)
        if ((e->alts[a]->rhs.guards[i] && has_let(e->alts[a]->rhs.guards[i])) || has_let(e->alts[a]->rhs.bodies[i]))
          return true;
    return false;
  case E_ANN: return has_let(e->body);
  default: return false;
  }
}

/* Evaluating e has no effect, cannot fail and costs nothing to repeat,
   so it may be substituted. `lazy` are let- and where-bound names, whose
   first use forces a thunk. */
static bool is_value(Expr *e, Nm *loc, Nm *lazy) {
  switch (e->k) {
  case E_VAR: return e->global_ref || !in(lazy, e->name);
  case E_INT: case E_FLOAT: case E_CHAR: case E_STR: case E_CON: case E_LAM: return true;
  case E_APP: {
    for (int i = 0; i < e->n; i++)
      if (!is_value(e->args[i], loc, lazy)) return false;
    if (e->fn->k == E_CON) return true;
    int g = gref(e->fn, loc);
    if (g >= 0 && CS->globals.a[g].prim >= 0) return e->n < fk_prims[CS->globals.a[g].prim].arity;
    Bind *b = gbind(g);
    return b && e->n < b->e->lam->n;
  }
  case E_ANN: return is_value(e->body, loc, lazy);
  default: return false;
  }
}

/* -------------------------------------------------------- pipelines */

static const char *const core_name[F_N] = {"", "map", "filter", "take", "takeWhile", "concat", "foldl", "any", "all",
                                            "enumFromTo", "replicate"};

static int core_kind(Bind *b) {
  if (b->from) b = b->from;
  for (int k = 1; k < F_N; k++)
    if (ST->core[k] == b) return k;
  return F_NONE;
}

typedef struct Pipe Pipe;

typedef struct {
  int kind;
  Expr *fn, *n;  /* map/filter/takeWhile/any/all/foldl: the function; take: count; foldl: start */
  Type *out;     /* element (or, for a consumer, result) type after the stage */
  int cnt;       /* take: its counter parameter */
  const char *v; /* concat: the element variable; inner: the list made from it */
  Pipe *inner;
} Stage;

struct Pipe {
  int kind;     /* F_RANGE, F_REPLICATE, or F_NONE: a list traversed */
  Expr *a, *b;  /* bounds; count and element; the list */
  Type *elem;
  VEC(Stage) st; /* outermost first, as parsed */
};

/* Unfold a call of a prelude function whose body is one call using its
   last parameter once: `sum xs = foldl (+) 0 xs`. */
static Expr *unfold(Expr *e, Nm *loc, Nm *lazy) {
  Bind *b = gbind(gref(e->fn, loc));
  if (!b || core_kind(b) || e->n != b->e->lam->n || !(b->from ? b->from : b)->prelude) return NULL;
  Lambda *L = b->e->lam;
  Expr *body = L->body;
  while (body->k == E_ANN) body = body->body;
  Nm *ps = NULL;
  for (int i = 0; i < L->n; i++) ps = nm(ps, L->params[i]);
  if (body->k != E_APP || !gbind(gref(body->fn, ps)) || has_let(body)) return NULL;
  if (occ(body, L->params[L->n - 1]) != 1) return NULL; /* the list, once, as an argument */
  Nm *bound = binders(body, NULL);
  for (int i = 0; i < e->n; i++) {
    if (i < e->n - 1 && !is_value(e->args[i], loc, lazy)) return NULL;
    if (uses_any(e->args[i], bound)) return NULL; /* a binder of the body would capture it */
  }
  Expr *c = copy_expr(body);
  pin(c, ps);
  const char **tmp = xmalloc((size_t)L->n * sizeof *tmp); /* fresh names first: an argument may use a parameter's name */
  for (int i = 0; i < L->n; i++) c = subst_in(c, L->params[i], evar(tmp[i] = fresh(), e->args[i]->ty));
  for (int i = 0; i < L->n; i++) c = subst_in(c, tmp[i], e->args[i]);
  c->ty = e->ty;
  return c;
}

static Pipe *parse(Expr *e, Nm *loc, Nm *lazy, bool consumer);

/* concat (map (\v -> body) xs): the inner pipeline, from body. */
static bool concat_map(Stage *s, Expr *m, Nm *loc, Nm *lazy, Expr **src) {
  Bind *b = gbind(gref(m->fn, loc));
  if (!b || core_kind(b) != F_MAP || m->n != 2 || m->args[0]->k != E_LAM || m->args[0]->lam->n != 1) return false;
  Lambda *F = m->args[0]->lam;
  const char *v = fresh();
  Type *vt = F->ptypes ? F->ptypes[0] : elem_of(m->args[1]->ty);
  Pipe *in = parse(subst(F->body, F->params[0], evar(v, vt)), nm(loc, v), lazy, false);
  if (!in) return false;
  s->v = v;
  s->inner = in;
  *src = m->args[1];
  return true;
}

static Pipe *parse(Expr *e, Nm *loc, Nm *lazy, bool consumer) {
  Pipe *p = NEW(Pipe);
  for (int depth = 0; depth < 64; depth++) {
    while (e->k == E_ANN) e = e->body;
    Bind *b = e->k == E_APP ? gbind(gref(e->fn, loc)) : NULL;
    int k = b ? core_kind(b) : F_NONE;
    if (b && !k) {
      Expr *u = unfold(e, loc, lazy);
      if (u) {
        e = u;
        continue;
      }
    }
    if (b && k && e->n == b->e->lam->n) {
      Stage s = {.kind = k};
      switch (k) {
      case F_FOLDL:
      case F_ANY:
      case F_ALL:
        if (!consumer || p->st.n) break;
        s.fn = e->args[0];
        if (k == F_FOLDL) s.n = e->args[1];
        s.out = e->ty;
        VPUSH(p->st, s);
        e = e->args[e->n - 1];
        continue;
      case F_MAP:
      case F_FILTER:
      case F_TAKEWHILE:
      case F_TAKE:
        if (k == F_TAKE) s.n = e->args[0];
        else s.fn = e->args[0];
        s.out = elem_of(e->ty);
        VPUSH(p->st, s);
        e = e->args[1];
        continue;
      case F_CONCAT: {
        s.out = elem_of(e->ty);
        Expr *src = e->args[0];
        Stage inner = s;
        bool cm = false;
        for (int i = 0; i < p->st.n; i++) cm |= p->st.a[i].kind == F_CONCAT;
        if (cm) break; /* one level of nesting */
        Expr *m = src;
        while (m->k == E_ANN) m = m->body;
        if (m->k == E_APP && concat_map(&inner, m, loc, lazy, &src)) {
          bool nested = false;
          for (int i = 0; i < inner.inner->st.n; i++) nested |= inner.inner->st.a[i].kind == F_CONCAT;
          if (nested) break;
        } else { /* concat xss: each element is a list traversed */
          inner.v = fresh();
          inner.inner = NEW(Pipe);
          inner.inner->a = evar(inner.v, elem_of(src->ty));
          inner.inner->elem = s.out;
        }
        VPUSH(p->st, inner);
        e = src;
        continue;
      }
      case F_RANGE:
        if (!elem_of(e->ty) || repr(elem_of(e->ty))->k != TY_CON || strcmp(repr(elem_of(e->ty))->name, "Int")) break;
        p->kind = F_RANGE;
        p->a = e->args[0];
        p->b = e->args[1];
        p->elem = elem_of(e->ty);
        return p;
      case F_REPLICATE:
        p->kind = F_REPLICATE;
        p->a = e->args[0];
        p->b = e->args[1];
        p->elem = elem_of(e->ty);
        return p;
      }
    }
    p->kind = F_NONE;
    p->a = e;
    p->elem = elem_of(e->ty);
    return p->elem ? p : NULL;
  }
  return NULL;
}

/* ------------------------------------------------------- generation */

typedef struct {
  const char *loop;
  Type *res;
  VEC(const char *) pn; /* loop parameters */
  VEC(Type *) pt;
  Expr **init;          /* their values at entry */
  /* level 0: producer state s0; stages A (producer side first), the
     concat stage, R after it, and the consumer, or none: materialize */
  Pipe *top;
  Stage **A, **R, *cm, *cons;
  int nA, nR, s0;
  Expr *hi0;
  /* level 1 */
  Stage **B;
  int nB, s1, hi1, iv, nset1, *iset1, iacc;
  Expr **set1;
  /* setup of level 0: case-bound before the loop, in evaluation order */
  VEC(const char *) sn;
  VEC(Expr *) se;
} Gen;

static int param(Gen *g, const char *name, Type *t) {
  VPUSH(g->pn, name);
  VPUSH(g->pt, t);
  return g->pn.n - 1;
}

static Expr **cur_copy(Gen *g, Expr **cur) {
  Expr **c = xmalloc((size_t)g->pn.n * sizeof *c + 1);
  copy_n(c, cur, (size_t)g->pn.n * sizeof *c);
  return c;
}

static Expr *pv(Gen *g, int i) { return evar(g->pn.a[i], g->pt.a[i]); }

static Type *loop_type(Gen *g) {
  Type *t = g->res;
  for (int i = g->pn.n; i-- > 0;) t = t_fun(g->pt.a[i], t);
  return t;
}

static Expr *cont(Gen *g, Expr **cur) {
  Expr **a = xmalloc((size_t)g->pn.n * sizeof *a + 1);
  for (int i = 0; i < g->pn.n; i++) a[i] = copy_expr(cur[i]);
  return eapp(evar(g->loop, loop_type(g)), g->pn.n, a, g->res);
}

static Expr *done(Gen *g, Expr **cur) {
  if (!g->cons) return econ(FK_C_NIL, "[]", g->res);
  if (g->cons->kind == F_FOLDL) return copy_expr(cur[g->iacc]);
  return ebool(g->cons->kind == F_ALL);
}

/* A template, setup variable or value, copied for one use. */
static Expr *use(Expr *t) { return copy_expr(t); }

static Expr *step(Gen *g, int lv, Expr **cur);
static Expr *flow(Gen *g, int lv, int j, Expr *y, Expr **cur);

/* The stream of level lv ended. */
static Expr *end(Gen *g, int lv, Expr **cur) {
  if (lv == 0) return done(g, cur);
  Expr **c = cur_copy(g, cur);
  Pipe *p = g->cm->inner;
  if (p->kind == F_NONE) c[g->s1] = econ(FK_C_NIL, "[]", p->a->ty);
  else c[g->s1] = p->kind == F_RANGE ? eint(1) : eint(0);
  if (p->kind == F_RANGE) c[g->hi1] = eint(0);
  return step(g, 0, c);
}

static Expr *step(Gen *g, int lv, Expr **cur) {
  Pipe *p = lv ? g->cm->inner : g->top;
  int s = lv ? g->s1 : g->s0;
  Expr **c = cur_copy(g, cur);
  switch (p->kind) {
  case F_RANGE: {
    Expr *hi = lv ? cur[g->hi1] : g->hi0;
    c[s] = eprim("primAddInt", copy_expr(cur[s]), eint(1), t_int());
    return eif(eprim("primGtInt", copy_expr(cur[s]), use(hi), t_bool()), end(g, lv, cur), flow(g, lv, 0, copy_expr(cur[s]), c), g->res);
  }
  case F_REPLICATE:
    c[s] = eprim("primSubInt", copy_expr(cur[s]), eint(1), t_int());
    return eif(eprim("primLeInt", copy_expr(cur[s]), eint(0), t_bool()), end(g, lv, cur), flow(g, lv, 0, use(p->b), c), g->res);
  default: {
    const char *x = fresh(), *rest = fresh();
    c[s] = evar(rest, cur[s]->ty);
    return ematch(copy_expr(cur[s]), end(g, lv, cur), x, rest, flow(g, lv, 0, evar(x, p->elem), c));
  }
  }
}

/* Stage j of the flow of an element y of level lv: level 0 runs A, then
   the concat stage or the consumer; level 1 runs B, R, the consumer. */
static Expr *flow(Gen *g, int lv, int j, Expr *y, Expr **cur) {
  Stage *s;
  int origin = lv;
  if (lv == 0) s = j < g->nA ? g->A[j] : g->cm ? g->cm : g->cons;
  else if (j < g->nB) s = g->B[j];
  else {
    s = j - g->nB < g->nR ? g->R[j - g->nB] : g->cons;
    origin = 0;
  }
  if (!s) return eapp(econ(FK_C_CONS, ":", t_fun(y->ty, t_fun(g->res, g->res))), 2, (Expr *[]){y, cont(g, cur)}, g->res);
  Expr **c = cur_copy(g, cur);
  switch (s->kind) {
  case F_MAP: {
    const char *z = fresh();
    return ebind(eapp1(use(s->fn), y, s->out), z, flow(g, lv, j + 1, evar(z, s->out), cur));
  }
  case F_FILTER:
    return eif(eapp1(use(s->fn), y, t_bool()), flow(g, lv, j + 1, copy_expr(y), cur), cont(g, cur), g->res);
  case F_TAKEWHILE:
    return eif(eapp1(use(s->fn), y, t_bool()), flow(g, lv, j + 1, copy_expr(y), cur), end(g, origin, cur), g->res);
  case F_TAKE:
    c[s->cnt] = eprim("primSubInt", copy_expr(cur[s->cnt]), eint(1), t_int());
    return eif(eprim("primLeInt", copy_expr(cur[s->cnt]), eint(0), t_bool()), end(g, origin, cur),
               flow(g, lv, j + 1, y, c), g->res);
  case F_CONCAT: { /* start level 1 on y */
    Pipe *p = s->inner;
    c[g->iv] = evar(s->v, y->ty);
    for (int i = 0; i < g->nset1; i++) c[g->iset1[i]] = pv(g, g->iset1[i]);
    c[g->s1] = use(p->a);
    if (p->kind == F_RANGE) c[g->hi1] = use(p->b);
    for (int i = 0; i < g->nB; i++)
      if (g->B[i]->kind == F_TAKE) c[g->B[i]->cnt] = use(g->B[i]->n);
    Expr *body = cont(g, c);
    for (int i = g->nset1; i-- > 0;) body = ebind(g->set1[i], g->pn.a[g->iset1[i]], body);
    return ebind(y, s->v, body);
  }
  case F_FOLDL:
    c[g->iacc] = eapp(use(s->fn), 2, (Expr *[]){copy_expr(cur[g->iacc]), y}, s->out);
    return cont(g, c);
  case F_ANY: return eif(eapp1(use(s->fn), y, t_bool()), ebool(true), cont(g, cur), g->res);
  case F_ALL: return eif(eapp1(use(s->fn), y, t_bool()), cont(g, cur), ebool(false), g->res);
  default: abort();
  }
}

/* -------------------------------------------------------- the pass */

typedef struct { Expr *fn; bool expr; int at; } Item;
typedef VEC(Expr *) Exprs; /* per-element work, in flow order */

/* A template for e: e itself if a value, else a setup variable. */
static Expr *setup(Gen *g, Expr *e, int level, Exprs *s1) {
  if (is_value(e, ST->floc, ST->flazy)) return e;
  if (level == 0) {
    const char *x = fresh();
    VPUSH(g->sn, x);
    VPUSH(g->se, e);
    return evar(x, e->ty);
  }
  VPUSH(*s1, e);
  return NULL; /* filled in once the parameter exists */
}

static bool fuse_at(Expr *e, Nm *loc, Nm *lazy) {
  Pipe *top = parse(e, loc, lazy, true);
  if (!top) return false;
  Gen g = {.top = top};
  int nst = top->st.n;
  Stage **st = xmalloc((size_t)nst * sizeof *st + 1); /* producer side first */
  for (int i = 0; i < nst; i++) st[i] = &top->st.a[nst - 1 - i];
  int ncm = -1;
  for (int i = 0; i < nst; i++)
    if (st[i]->kind == F_CONCAT) ncm = i;
  if (nst && (st[nst - 1]->kind == F_FOLDL || st[nst - 1]->kind == F_ANY || st[nst - 1]->kind == F_ALL)) {
    g.cons = st[nst - 1];
    nst--;
  }
  g.A = st;
  g.nA = ncm >= 0 ? ncm : nst;
  if (ncm >= 0) {
    g.cm = st[ncm];
    g.R = st + ncm + 1;
    g.nR = nst - ncm - 1;
    Pipe *in = g.cm->inner;
    g.nB = in->st.n;
    g.B = xmalloc((size_t)g.nB * sizeof *g.B + 1);
    for (int i = 0; i < g.nB; i++) g.B[i] = &in->st.a[g.nB - 1 - i];
  }
  /* At least one intermediate list must go. */
  int work = nst + (g.cons != NULL) + (top->kind != F_NONE) + (g.cm ? g.nB + (g.cm->inner->kind != F_NONE) : 0);
  if (work < 2) return false;
  /* The rules: no effects, at most one failing stage, none failing before
     a short circuit. Items in flow order. */
  VEC(Item) items = {0};
  VEC(int) shorts = {0}; /* index into items where a short circuit sits */
  Nm *iloc = g.cm ? nm(loc, g.cm->v) : loc;
  for (int i = 0; i < g.nA; i++) {
    if (g.A[i]->kind == F_TAKE || g.A[i]->kind == F_TAKEWHILE) VPUSH(shorts, items.n);
    if (g.A[i]->fn) VPUSH(items, ((Item){g.A[i]->fn, false, 0}));
  }
  if (g.cm) {
    Pipe *in = g.cm->inner;
    Expr *ie[2] = {in->a, in->b};
    for (int i = 0; i < 2; i++)
      if (ie[i] && !is_value(ie[i], iloc, lazy)) VPUSH(items, ((Item){ie[i], true, 1}));
    for (int i = 0; i < g.nB; i++) {
      Stage *s = g.B[i];
      if (s->n && !is_value(s->n, iloc, lazy)) VPUSH(items, ((Item){s->n, true, 1}));
      if (s->kind == F_TAKE || s->kind == F_TAKEWHILE) VPUSH(shorts, items.n);
      if (s->fn) VPUSH(items, ((Item){s->fn, false, 1}));
    }
    for (int i = 0; i < g.nR; i++) {
      if (g.R[i]->kind == F_TAKE || g.R[i]->kind == F_TAKEWHILE) VPUSH(shorts, items.n);
      if (g.R[i]->fn) VPUSH(items, ((Item){g.R[i]->fn, false, 0}));
    }
  }
  if (g.cons) {
    if (g.cons->kind != F_FOLDL) VPUSH(shorts, items.n);
    VPUSH(items, ((Item){g.cons->fn, false, 0}));
  }
  bool *total = xcalloc((size_t)items.n + 1, 1);
  int failing = 0;
  for (int i = 0; i < items.n; i++) {
    Item *it = &items.a[i];
    Nm *l = it->at ? iloc : loc;
    if (it->expr ? !ok_e(it->fn, l, NULL, false) : (!is_value(it->fn, l, lazy) || !fn_ok(it->fn, l, false))) return false;
    total[i] = it->expr ? ok_e(it->fn, l, NULL, true) : fn_ok(it->fn, l, true);
    failing += !total[i];
  }
  if (failing > 1) return false;
  for (int k = 0; k < shorts.n; k++)
    for (int i = 0; i < shorts.a[k]; i++)
      if (!total[i]) return false;
  /* Parameters and their values at entry, setup in evaluation order:
     outermost stage first, producer last. */
  ST->floc = loc;
  ST->flazy = lazy;
  VEC(Expr *) init = {0};
  g.loop = fresh();
  g.res = e->ty;
  if (g.cons) {
    g.cons->fn = setup(&g, g.cons->fn, 0, NULL);
    if (g.cons->kind == F_FOLDL) g.cons->n = setup(&g, g.cons->n, 0, NULL);
  }
  for (int i = g.nR; i-- > 0;) {
    Stage *s = g.R[i];
    if (s->n) s->n = setup(&g, s->n, 0, NULL);
  }
  for (int i = g.nA; i-- > 0;) {
    Stage *s = g.A[i];
    if (s->n) s->n = setup(&g, s->n, 0, NULL);
  }
  Expr *a0 = setup(&g, top->a, 0, NULL), *b0 = top->b ? setup(&g, top->b, 0, NULL) : NULL;
  g.s0 = param(&g, fresh(), top->kind == F_NONE ? top->a->ty : t_int());
  VPUSH(init, a0);
  if (top->kind == F_RANGE) g.hi0 = b0;
  if (top->kind == F_REPLICATE) top->b = b0;
  for (int i = 0; i < g.nA; i++)
    if (g.A[i]->kind == F_TAKE) {
      g.A[i]->cnt = param(&g, fresh(), t_int());
      VPUSH(init, g.A[i]->n);
    }
  for (int i = 0; i < g.nR; i++)
    if (g.R[i]->kind == F_TAKE) {
      g.R[i]->cnt = param(&g, fresh(), t_int());
      VPUSH(init, g.R[i]->n);
    }
  if (g.cm) {
    Pipe *in = g.cm->inner;
    ST->floc = iloc;
    g.iv = param(&g, g.cm->v, g.nA ? g.A[g.nA - 1]->out : top->elem); /* the outer elements' type */
    /* Per outer element, in evaluation order: outermost stage first. */
    Exprs s1 = {0};
    VEC(Expr **) slots = {0};
    Expr **at[16];
    int nat = 0;
    if (g.nB > 12) return false;
    for (int i = g.nB; i-- > 0;)
      if (g.B[i]->n) at[nat++] = &g.B[i]->n;
    at[nat++] = &in->a;
    if (in->b) at[nat++] = &in->b;
    for (int i = 0; i < nat; i++) {
      Expr *x = setup(&g, *at[i], 1, &s1);
      if (x) *at[i] = x;
      else VPUSH(slots, at[i]);
    }
    /* Setup of level 1 runs per outer element: in flow order, outermost
       stage first, as parsed; these become parameters. */
    g.nset1 = s1.n;
    g.set1 = s1.a;
    g.iset1 = xmalloc((size_t)s1.n * sizeof *g.iset1 + 1);
    for (int i = 0; i < s1.n; i++) {
      g.iset1[i] = param(&g, fresh(), s1.a[i]->ty);
      *slots.a[i] = pv(&g, g.iset1[i]);
    }
    g.s1 = param(&g, fresh(), in->kind == F_NONE ? in->a->ty : t_int());
    if (in->kind == F_RANGE) g.hi1 = param(&g, fresh(), t_int());
    for (int i = 0; i < g.nB; i++)
      if (g.B[i]->kind == F_TAKE) g.B[i]->cnt = param(&g, fresh(), t_int());
    ST->floc = loc;
  }
  if (g.cons && g.cons->kind == F_FOLDL) {
    g.iacc = param(&g, fresh(), g.cons->out);
    VPUSH(init, g.cons->n);
  }
  /* Entry values: level 1 starts exhausted. */
  Expr **ent = xmalloc((size_t)g.pn.n * sizeof *ent + 1);
  int k = 0;
  for (int i = 0; i < g.pn.n; i++) ent[i] = NULL;
  ent[g.s0] = init.a[k++];
  for (int i = 0; i < g.nA; i++)
    if (g.A[i]->kind == F_TAKE) ent[g.A[i]->cnt] = init.a[k++];
  for (int i = 0; i < g.nR; i++)
    if (g.R[i]->kind == F_TAKE) ent[g.R[i]->cnt] = init.a[k++];
  if (g.cons && g.cons->kind == F_FOLDL) ent[g.iacc] = init.a[k++];
  if (g.cm) {
    Pipe *in = g.cm->inner;
    for (int i = 0; i < g.pn.n; i++)
      if (!ent[i] && !(ent[i] = dummy(g.pt.a[i]))) return false;
    if (in->kind == F_RANGE) {
      ent[g.s1] = eint(1);
      ent[g.hi1] = eint(0);
    } else if (in->kind == F_REPLICATE) {
      ent[g.s1] = eint(0);
    }
  }
  /* The loop. */
  Expr **cur = xmalloc((size_t)g.pn.n * sizeof *cur + 1);
  for (int i = 0; i < g.pn.n; i++) cur[i] = pv(&g, i);
  Expr *body = step(&g, g.cm ? 1 : 0, cur);
  Lambda *L = NEW(Lambda);
  L->n = g.pn.n;
  L->params = g.pn.a;
  L->ptypes = g.pt.a;
  L->body = body;
  L->line = ST->fline;
  L->col = ST->fcol;
  Expr *lam = mk(E_LAM, loop_type(&g));
  lam->lam = L;
  Bind *lb = NEW(Bind);
  *lb = (Bind){.name = g.loop, .e = lam, .line = ST->fline, .col = ST->fcol, .type = lam->ty};
  Expr *let = mk(E_LET, g.res);
  let->g.b = xmalloc(sizeof *let->g.b);
  let->g.b[0] = lb;
  let->g.n = 1;
  let->g.rec = true;
  let->body = cont(&g, ent);
  Expr *out = let;
  for (int i = g.sn.n; i-- > 0;) out = ebind(g.se.a[i], g.sn.a[i], out);
  Type *t = e->ty;
  *e = *out;
  e->ty = t;
  return true;
}

static void fuse_expr(Expr *e, Nm *loc, Nm *lazy);

static void fuse_rhs(Rhs *r, Nm *loc, Nm *lazy) {
  for (int i = 0; i < r->nwg; i++) {
    loc = group_nm(&r->wg[i], loc);
    lazy = group_nm(&r->wg[i], lazy);
    for (int j = 0; j < r->wg[i].n; j++) fuse_expr(r->wg[i].b[j]->e, loc, lazy);
  }
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) fuse_expr(r->guards[i], loc, lazy);
    fuse_expr(r->bodies[i], loc, lazy);
  }
}


static void fuse_expr(Expr *e, Nm *loc, Nm *lazy) {
  if (e->k == E_APP) {
    ST->fline = e->line;
    ST->fcol = e->col;
    if (fuse_at(e, loc, lazy)) ST->changed = true;
  }
  switch (e->k) {
  case E_APP:
    fuse_expr(e->fn, loc, lazy);
    for (int i = 0; i < e->n; i++) fuse_expr(e->args[i], loc, lazy);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) loc = nm(loc, e->lam->params[i]);
    fuse_expr(e->lam->body, loc, lazy);
    return;
  case E_LET:
    loc = group_nm(&e->g, loc);
    lazy = group_nm(&e->g, lazy);
    for (int i = 0; i < e->g.n; i++) fuse_expr(e->g.b[i]->e, loc, lazy);
    fuse_expr(e->body, loc, lazy);
    return;
  case E_IF:
    fuse_expr(e->cond, loc, lazy);
    fuse_expr(e->then_, loc, lazy);
    fuse_expr(e->else_, loc, lazy);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) fuse_expr(e->scruts[i], loc, lazy);
    for (int a = 0; a < e->na; a++) {
      Nm *t = loc;
      for (int i = 0; i < e->alts[a]->np; i++) t = pat_nm(e->alts[a]->pats[i], t);
      fuse_rhs(&e->alts[a]->rhs, t, lazy);
    }
    return;
  case E_ANN: fuse_expr(e->body, loc, lazy); return;
  default: return;
  }
}

/* Fuse pipelines in the chunk's bindings; returns how many were changed
   and moves them to the front of all, for lift_locals. */
int fuse_lists(Bind **all, int n) {
  bool prelude = !strcmp(CS->cur_file, "<prelude>");
  VEC(Bind *) fns = {0};
  for (int i = 0; i < n; i++) {
    Bind *b = all[i];
    if (b->e->k == E_LAM && !CS->globals.a[b->gidx].bind) CS->globals.a[b->gidx].bind = b; /* type-class clones */
    b->prelude = prelude;
    if (prelude)
      for (int k = 1; k < F_N; k++)
        if (b->name == intern(core_name[k])) ST->core[k] = b;
    if (b->e->k == E_LAM && b->e->lam->ptypes) VPUSH(fns, b);
  }
  analyze(fns.a, fns.n);
  if (getenv("FK_NOFUSE")) return 0;
  int nc = 0;
  for (int i = 0; i < n; i++) {
    ST->changed = false;
    Nm *loc = NULL;
    if (all[i]->e->k == E_LAM)
      for (int j = 0; j < all[i]->e->lam->n; j++) loc = nm(loc, all[i]->e->lam->params[j]);
    fuse_expr(all[i]->e->k == E_LAM ? all[i]->e->lam->body : all[i]->e, loc, NULL);
    if (ST->changed) {
      Bind *t = all[nc];
      all[nc++] = all[i];
      all[i] = t;
    }
  }
  return nc;
}
