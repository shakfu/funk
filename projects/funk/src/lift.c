/* lift.c -- lambda lifting, after dictionary passing (elab.h).

   A local function that is only ever called -- never passed, returned or
   stored -- needs no closure. It becomes a top-level function taking its
   free local variables as leading parameters, and each call passes them:
   `safe q qs = check qs 1 where check (c:cs) d = .. q ..` calls
   `safe/check q qs 1`. Lifted functions are then ordinary globals: direct
   calls, specialization, TRMC. A group is lifted only if no binder in its
   region rebinds one of its free variables, so the variables passed at a
   call are the ones the function used. */
#include "elab.h"

typedef struct Lift { const char *name, *global; int nfv; const char **fv; Type **fvt; struct Lift *next; } Lift;


static Lift *lift_find(Lift *l, const char *name) {
  for (; l; l = l->next)
    if (l->name == name) return l;
  return NULL;
}

/* Does name occur in e other than as the function of a call with at least
   arity arguments? */
static bool escapes(Expr *e, const char *name, int arity);

static bool escapes_group(Group *g, const char *name, int arity) {
  for (int i = 0; i < g->n; i++)
    if (escapes(g->b[i]->e, name, arity)) return true;
  return false;
}

static bool escapes(Expr *e, const char *name, int arity) {
  switch (e->k) {
  case E_VAR: return e->name == name && !e->global_ref;
  case E_APP:
    if (!(e->fn->k == E_VAR && e->fn->name == name && !e->fn->global_ref && e->n >= arity) &&
        escapes(e->fn, name, arity))
      return true;
    for (int i = 0; i < e->n; i++)
      if (escapes(e->args[i], name, arity)) return true;
    return false;
  case E_LAM: return escapes(e->lam->body, name, arity);
  case E_LET: return escapes_group(&e->g, name, arity) || escapes(e->body, name, arity);
  case E_IF: return escapes(e->cond, name, arity) || escapes(e->then_, name, arity) || escapes(e->else_, name, arity);
  case E_CASE:
    for (int i = 0; i < e->ns; i++)
      if (escapes(e->scruts[i], name, arity)) return true;
    for (int a = 0; a < e->na; a++) {
      Rhs *r = &e->alts[a]->rhs;
      for (int i = 0; i < r->nwg; i++)
        if (escapes_group(&r->wg[i], name, arity)) return true;
      for (int i = 0; i < r->ng; i++)
        if ((r->guards[i] && escapes(r->guards[i], name, arity)) || escapes(r->bodies[i], name, arity)) return true;
    }
    return false;
  case E_ANN: return escapes(e->body, name, arity);
  default: return false;
  }
}

/* All names bound anywhere in e (parameters, patterns, bindings). */
static Nm *binders(Expr *e, Nm *s);

static Nm *binders_group(Group *g, Nm *s) {
  for (int i = 0; i < g->n; i++) s = binders(g->b[i]->e, el_nm(s, g->b[i]->name));
  return s;
}

static Nm *binders(Expr *e, Nm *s) {
  switch (e->k) {
  case E_APP:
    s = binders(e->fn, s);
    for (int i = 0; i < e->n; i++) s = binders(e->args[i], s);
    return s;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) s = el_nm(s, e->lam->params[i]);
    return binders(e->lam->body, s);
  case E_LET: return binders(e->body, binders_group(&e->g, s));
  case E_IF: return binders(e->else_, binders(e->then_, binders(e->cond, s)));
  case E_CASE:
    for (int i = 0; i < e->ns; i++) s = binders(e->scruts[i], s);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      for (int i = 0; i < alt->np; i++) s = el_pat_nm(alt->pats[i], s);
      for (int i = 0; i < alt->rhs.nwg; i++) s = binders_group(&alt->rhs.wg[i], s);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) s = binders(alt->rhs.guards[i], s);
        s = binders(alt->rhs.bodies[i], s);
      }
    }
    return s;
  case E_ANN: return binders(e->body, s);
  default: return s;
  }
}

/* Free local variables of e: names in locals, not bound inside e, plus
   the free variables of lifted functions it calls. */
static void free_locals(Expr *e, Nm *inner, Nm *locals, Lift *lifts, Fv *fv);

static void add_fv(Fv *fv, const char *name, Type *t) {
  if (el_fv_index(fv, name) >= 0) return;
  VPUSH(fv->names, name);
  VPUSH(fv->types, t);
}

static Nm *free_group(Group *g, Nm *inner, Nm *locals, Lift *lifts, Fv *fv) {
  for (int i = 0; i < g->n; i++) inner = el_nm(inner, g->b[i]->name);
  for (int i = 0; i < g->n; i++) free_locals(g->b[i]->e, inner, locals, lifts, fv);
  return inner;
}

static void free_locals(Expr *e, Nm *inner, Nm *locals, Lift *lifts, Fv *fv) {
  switch (e->k) {
  case E_VAR: {
    if (e->global_ref || el_in(inner, e->name)) return;
    Lift *l = lift_find(lifts, e->name);
    if (l) {
      for (int i = 0; i < l->nfv; i++) add_fv(fv, l->fv[i], l->fvt[i]);
      return;
    }
    if (el_in(locals, e->name)) add_fv(fv, e->name, e->ty);
    return;
  }
  case E_APP:
    free_locals(e->fn, inner, locals, lifts, fv);
    for (int i = 0; i < e->n; i++) free_locals(e->args[i], inner, locals, lifts, fv);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) inner = el_nm(inner, e->lam->params[i]);
    free_locals(e->lam->body, inner, locals, lifts, fv);
    return;
  case E_LET: free_locals(e->body, free_group(&e->g, inner, locals, lifts, fv), locals, lifts, fv); return;
  case E_IF:
    free_locals(e->cond, inner, locals, lifts, fv);
    free_locals(e->then_, inner, locals, lifts, fv);
    free_locals(e->else_, inner, locals, lifts, fv);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) free_locals(e->scruts[i], inner, locals, lifts, fv);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = inner;
      for (int i = 0; i < alt->np; i++) t = el_pat_nm(alt->pats[i], t);
      for (int i = 0; i < alt->rhs.nwg; i++) t = free_group(&alt->rhs.wg[i], t, locals, lifts, fv);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) free_locals(alt->rhs.guards[i], t, locals, lifts, fv);
        free_locals(alt->rhs.bodies[i], t, locals, lifts, fv);
      }
    }
    return;
  case E_ANN: free_locals(e->body, inner, locals, lifts, fv); return;
  default: return;
  }
}


static void lift_expr(Expr *e, Nm *locals, Lift *lifts, const char *owner);

/* Try to lift group g, whose scope also covers the expressions in rest.
   Returns the lifts in scope afterwards; *done tells whether g was
   lifted (and must be removed by the caller). */
static Lift *lift_group(Group *g, Expr **rest, int nrest, Nm *locals, Lift *lifts, const char *owner, bool *done) {
  *done = false;
  for (int i = 0; i < g->n; i++)
    if (g->b[i]->e->k != E_LAM || !g->b[i]->e->lam->ptypes) return lifts;
  for (int i = 0; i < g->n; i++) {
    const char *x = g->b[i]->name;
    int ar = g->b[i]->e->lam->n;
    if (escapes_group(g, x, ar)) return lifts;
    for (int k = 0; k < nrest; k++)
      if (rest[k] && escapes(rest[k], x, ar)) return lifts;
  }
  Fv fv = {0};
  for (int i = 0; i < g->n; i++) {
    Nm *inner = NULL;
    for (int k = 0; k < g->n; k++) inner = el_nm(inner, g->b[k]->name);
    free_locals(g->b[i]->e, inner, locals, lifts, &fv);
  }
  if (fv.names.n > 8) return lifts;
  Nm *bound = binders_group(g, NULL);
  for (int k = 0; k < nrest; k++)
    if (rest[k]) bound = binders(rest[k], bound);
  for (int i = 0; i < fv.names.n; i++)
    if (el_in(bound, fv.names.a[i])) return lifts; /* a call might see another variable of that name */
  for (int i = 0; i < g->n; i++) {
    Lift *l = NEW(Lift);
    *l = (Lift){g->b[i]->name, intern(xsprintf("%s/%s%d", owner, g->b[i]->name, ST->nlift++)), fv.names.n, fv.names.a,
                fv.types.a, lifts};
    lifts = l;
  }
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    Lambda *L = b->e->lam;
    Lift *l = lift_find(lifts, b->name);
    const char **ps = xmalloc((size_t)(l->nfv + L->n) * sizeof *ps);
    Type **ts = xmalloc((size_t)(l->nfv + L->n) * sizeof *ts);
    if (l->nfv) { /* an empty array may be NULL, which memcpy's arguments must not be */
      memcpy(ps, l->fv, (size_t)l->nfv * sizeof *ps);
      memcpy(ts, l->fvt, (size_t)l->nfv * sizeof *ts);
    }
    if (L->n) {
      memcpy(ps + l->nfv, L->params, (size_t)L->n * sizeof *ps);
      memcpy(ts + l->nfv, L->ptypes, (size_t)L->n * sizeof *ts);
    }
    L->params = ps;
    L->ptypes = ts;
    L->n += l->nfv;
    L->name = l->global;
    Nm *inner = NULL;
    for (int k = 0; k < L->n; k++) inner = el_nm(inner, L->params[k]);
    lift_expr(L->body, inner, lifts, l->global); /* the free variables are parameters now */
    Bind *nb = NEW(Bind);
    *nb = (Bind){.name = l->global, .e = b->e, .line = b->line, .col = b->col, .type = b->type};
    nb->gidx = global_add(l->global, NULL, L);
    CS->globals.a[nb->gidx].bind = nb;
    VPUSH(ST->lifted, nb);
  }
  *done = true;
  return lifts;
}

static void lift_call(Expr *e, Lift *lifts) {
  Expr *f = e->fn;
  Lift *l = f->k == E_VAR && !f->global_ref ? lift_find(lifts, f->name) : NULL;
  if (!l) return;
  Expr **all = xmalloc((size_t)(l->nfv + e->n) * sizeof *all);
  for (int i = 0; i < l->nfv; i++) {
    Expr *v = el_mk(E_VAR, e->line, e->col);
    v->name = l->fv[i];
    v->ty = l->fvt[i];
    all[i] = v;
  }
  copy_n(all + l->nfv, e->args, (size_t)e->n * sizeof *all);
  e->args = all;
  e->n += l->nfv;
  Type *t = f->ty;
  f->name = l->global;
  f->global_ref = true;
  f->ty = t;
}

static void lift_wgs(Rhs *r, Nm **locals, Lift **lifts, const char *owner) {
  for (int i = 0; i < r->nwg; i++) {
    VEC(Expr *) rest = {0};
    for (int k = i + 1; k < r->nwg; k++)
      for (int j = 0; j < r->wg[k].n; j++) VPUSH(rest, r->wg[k].b[j]->e);
    for (int k = 0; k < r->ng; k++) {
      VPUSH(rest, r->guards[k]);
      VPUSH(rest, r->bodies[k]);
    }
    bool done;
    *lifts = lift_group(&r->wg[i], rest.a, rest.n, *locals, *lifts, owner, &done);
    if (done) {
      move_n(r->wg + i, r->wg + i + 1, (size_t)(r->nwg - i - 1) * sizeof *r->wg);
      r->nwg--;
      i--;
      continue;
    }
    for (int j = 0; j < r->wg[i].n; j++) *locals = el_nm(*locals, r->wg[i].b[j]->name);
    for (int j = 0; j < r->wg[i].n; j++) lift_expr(r->wg[i].b[j]->e, *locals, *lifts, owner);
  }
}

static void lift_expr(Expr *e, Nm *locals, Lift *lifts, const char *owner) {
  switch (e->k) {
  case E_APP:
    lift_expr(e->fn, locals, lifts, owner);
    for (int i = 0; i < e->n; i++) lift_expr(e->args[i], locals, lifts, owner);
    lift_call(e, lifts);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) locals = el_nm(locals, e->lam->params[i]);
    lift_expr(e->lam->body, locals, lifts, owner);
    return;
  case E_LET: {
    bool done;
    Expr *rest[1] = {e->body};
    lifts = lift_group(&e->g, rest, 1, locals, lifts, owner, &done);
    if (done) {
      Type *t = e->ty;
      Expr *body = e->body;
      *e = *body;
      e->ty = t;
      lift_expr(e, locals, lifts, owner);
      return;
    }
    for (int i = 0; i < e->g.n; i++) locals = el_nm(locals, e->g.b[i]->name);
    for (int i = 0; i < e->g.n; i++) lift_expr(e->g.b[i]->e, locals, lifts, owner);
    lift_expr(e->body, locals, lifts, owner);
    return;
  }
  case E_IF:
    lift_expr(e->cond, locals, lifts, owner);
    lift_expr(e->then_, locals, lifts, owner);
    lift_expr(e->else_, locals, lifts, owner);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) lift_expr(e->scruts[i], locals, lifts, owner);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = locals;
      Lift *ls = lifts;
      for (int i = 0; i < alt->np; i++) t = el_pat_nm(alt->pats[i], t);
      lift_wgs(&alt->rhs, &t, &ls, owner);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) lift_expr(alt->rhs.guards[i], t, ls, owner);
        lift_expr(alt->rhs.bodies[i], t, ls, owner);
      }
    }
    return;
  case E_ANN: lift_expr(e->body, locals, lifts, owner); return;
  default: return;
  }
}

/* Lift local functions in bs; lifted functions go to `specialized`, and
   are themselves searched for nested ones. */
void lift_locals(Bind **bs, int n) {
  for (int i = 0; i < n; i++) lift_expr(bs[i]->e, NULL, NULL, bs[i]->name);
  for (int i = 0; i < ST->lifted.n; i++) VPUSH(CS->specialized, ST->lifted.a[i]);
  ST->lifted.n = 0;
}
