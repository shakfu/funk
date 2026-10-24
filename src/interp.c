/* interp.c -- AST interpreter over resolved code (REPL and `funk run`).

   Interpreted closures are ordinary runtime closures whose code pointer is
   interp_entry and whose info pointer is the Lambda, so fk_apply, PAPs and
   primitives treat them like compiled ones. A call in tail position to an
   interpreted closure of exact arity reuses the current frame, so
   tail-recursive loops run in constant C stack. */
#include "funk.h"

static V *gvals;
static size_t ngvals, capgvals;

/* Tail call target: args are written straight into the caller's frame. */
typedef struct { V *fr; int cap; V f; } Tail;

static V run(Lambda *L, V self, V *args);

static V interp_entry(V self, V *args) { return run(FK_INFO(self), self, args); }

V interp_global(int gidx) { return gvals[gidx]; }

static V load(Expr *e, V *fr, V self) {
  V v;
  switch (e->vk) {
  case VK_LOCAL: v = fr[e->idx]; break;
  case VK_FREE: v = FK_FREE(self, e->idx); break;
  default:
    v = gvals[e->idx];
    if (!v) fk_error("`%s` is undefined (its definition failed)", e->name);
    if (e->tdesc) return fk_typed_show_fn(fk_prims[globals.a[e->idx].prim].code == fk_p_print, e->tdesc);
    return v;
  }
  return e->lazy ? fk_force(v) : v;
}

static V alloc_closure(Lambda *L) { return fk_closure(interp_entry, L->n, L->nfree, L); }

static void fill_closure(V c, Lambda *L, V *fr, V self) {
  for (int i = 0; i < L->nfree; i++)
    FK_FREE(c, i) = L->free[i].vk == VK_LOCAL ? fr[L->free[i].idx] : FK_FREE(self, L->free[i].idx);
}

static V eval(Expr *e, V *fr, V self, Tail *tc);

static void eval_group(Group *g, V *fr, V self) {
  if (!g->rec) {
    Lambda *L = g->b[0]->thunk;
    if (!L) {
      fr[g->b[0]->slot] = eval(g->b[0]->e, fr, self, NULL);
      return;
    }
    V t = fk_thunk(interp_entry, L->nfree, L);
    fill_closure(t, L, fr, self);
    fr[g->b[0]->slot] = t;
    return;
  }
  for (int i = 0; i < g->n; i++) fr[g->b[i]->slot] = alloc_closure(g->b[i]->e->lam);
  for (int i = 0; i < g->n; i++) fill_closure(fr[g->b[i]->slot], g->b[i]->e->lam, fr, self);
}

static bool match(Pat *p, V v, V *fr) {
  switch (p->k) {
  case P_VAR: fr[p->slot] = v; return true;
  case P_WILD: return true;
  case P_INT: return fk_num_eq_int(v, p->ival);
  case P_CHAR: return v == FK_MKCHAR(p->ival);
  default:
    if (FK_CONID(v) != (uint32_t)p->con) return false;
    for (int i = 0; i < p->n; i++)
      if (!match(p->args[i], FK_FIELD(v, i), fr)) return false;
    return true;
  }
}

/* Select an alternative and return its body, to be evaluated by the
   caller in tail position. */
static Expr *eval_case(Expr *e, V *fr, V self) {
  V sv[e->ns + 1];
  for (int i = 0; i < e->ns; i++) sv[i] = eval(e->scruts[i], fr, self, NULL);
  for (int a = 0; a < e->na; a++) {
    Alt *alt = e->alts[a];
    bool ok = true;
    for (int i = 0; ok && i < alt->np; i++) ok = match(alt->pats[i], sv[i], fr);
    if (!ok) continue;
    Rhs *r = &alt->rhs;
    for (int i = 0; i < r->nwg; i++) eval_group(&r->wg[i], fr, self);
    for (int i = 0; i < r->ng; i++)
      if (!r->guards[i] || eval(r->guards[i], fr, self, NULL) == fk_true) return r->bodies[i];
  }
  fk_match_fail(xsprintf("%s:%d:%d", e->name, e->line, e->col));
}

/* With tc != NULL the expression is in tail position: a saturated call to
   an interpreted closure that fits the frame overwrites the frame with its
   args and returns 0 (never a valid value); run then loops. */
static V eval(Expr *e, V *fr, V self, Tail *tc) {
  for (;;) {
    switch (e->k) {
    case E_VAR: return load(e, fr, self);
    case E_CON: return fk_con_value((uint32_t)e->idx);
    case E_INT: return FK_MKINT(e->ival);
    case E_FLOAT: return fk_float(e->fval);
    case E_CHAR: return FK_MKCHAR(e->ival);
    case E_STR: return fk_string(e->name, (size_t)e->slen);
    case E_LAM: {
      V c = alloc_closure(e->lam);
      fill_closure(c, e->lam, fr, self);
      return c;
    }
    case E_ANN: e = e->body; continue;
    case E_IF: e = eval(e->cond, fr, self, NULL) == fk_true ? e->then_ : e->else_; continue;
    case E_LET: eval_group(&e->g, fr, self); e = e->body; continue;
    case E_CASE: e = eval_case(e, fr, self); continue;
    case E_APP: {
      int n = e->n;
      V args[n];
      if (e->fn->k == E_CON && fk_cons[e->fn->idx].arity == n) {
        for (int i = 0; i < n; i++) args[i] = eval(e->args[i], fr, self, NULL);
        return fk_mkcon((uint32_t)e->fn->idx, n, args);
      }
      V f = eval(e->fn, fr, self, NULL);
      for (int i = 0; i < n; i++) args[i] = eval(e->args[i], fr, self, NULL);
      if (FK_ISPTR(f) && FK_HDR(f)->kind == K_CLOSURE && FK_CODE(f) == interp_entry && (int)FK_ARITY(f) == n) {
        Lambda *L = FK_INFO(f);
        if (!tc || L->nslots > tc->cap) return run(L, f, args);
        memcpy(tc->fr, args, (size_t)n * sizeof(V));
        tc->f = f;
        return 0;
      }
      return fk_apply(f, n, args);
    }
    }
  }
}

static V run(Lambda *L, V self, V *args) {
  FK_STACK_CHECK();
  int cap = L->nslots > 8 ? L->nslots : 8;
  V fr[cap];
  if (L->n) memcpy(fr, args, (size_t)L->n * sizeof(V));
  Tail tc = {fr, cap, 0};
  for (;;) {
    V r = eval(L->body, fr, self, &tc);
    if (r) return r;
    self = tc.f;
    L = FK_INFO(self);
  }
}

void interp_install(Chunk *c) {
  if ((size_t)globals.n > capgvals) {
    size_t ncap = capgvals ? capgvals : 256;
    while (ncap < (size_t)globals.n) ncap *= 2;
    gvals = xrealloc(gvals, ncap * sizeof(V));
    memset(gvals + capgvals, 0, (ncap - capgvals) * sizeof(V));
    if (!capgvals) fk_add_roots(&gvals, &ngvals);
    capgvals = ncap;
  }
  for (size_t i = ngvals; i < (size_t)globals.n; i++)
    if (globals.a[i].prim >= 0) gvals[i] = fk_prim_value(globals.a[i].prim);
  ngvals = (size_t)globals.n;
  for (int i = 0; i < c->ng; i++) {
    Group *g = &c->groups[i];
    for (int j = 0; j < g->n; j++) {
      Bind *b = g->b[j];
      gvals[b->gidx] = b->init ? run(b->init, 0, NULL) : alloc_closure(b->e->lam);
    }
  }
}
