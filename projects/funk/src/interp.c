/* interp.c -- AST interpreter over resolved code (REPL and `funk run`).

   Interpreted closures are ordinary runtime closures whose code pointer is
   interp_entry and whose info pointer is the Lambda, so fk_apply, PAPs and
   primitives treat them like compiled ones. A call in tail position to an
   interpreted closure of exact arity reuses the current frame, so
   tail-recursive loops run in constant C stack. */
#include "funk.h"

/* interp.c's state, one per session (embedding.md, phase 2) */
typedef struct {
  V *gvals;
  size_t ngvals, capgvals;
  int interp_install_nexports; /* a foreign export is a C function: compiled code only */
} InterpState;
static const InterpState interp_init = {0};
#define ST ((InterpState *)fk_here()->cc[CC_INTERP])

void *interp_state_new(void) {
  InterpState *s = malloc(sizeof *s);
  if (!s) fk_fatal("out of memory");
  *s = interp_init;
  return s;
}


/* Tail call target: args are written straight into the caller's frame. */
typedef struct { V *fr; int cap; V f; } Tail;

static V run(Lambda *L, V self, V *args);

static V interp_entry(V self, V *args) { return run(FK_INFO(self), self, args); }

V interp_global(int gidx) { return ST->gvals[gidx]; }

static V load(Expr *e, V *fr, V self) {
  V v;
  switch (e->vk) {
  case VK_LOCAL: v = fr[e->idx]; break;
  case VK_FREE: v = FK_FREE(self, e->idx); break;
  default:
    v = ST->gvals[e->idx];
    if (!v) fk_error("`%s` is undefined (its definition failed)", e->name);
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
  case P_STR: return fk_str_eq_lit(v, p->name, (size_t)p->ival);
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
    case E_STR:
      if (!e->lit) e->lit = fk_str_static(e->name, (size_t)e->slen);
      return e->lit;
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
        copy_n(tc->fr, args, (size_t)n * sizeof(V));
        tc->f = f;
        return 0;
      }
      return fk_apply(f, n, args);
    }
    }
  }
}

static void check_interrupt(void) {
  if (atomic_load_explicit(&fk_interrupt, memory_order_relaxed)) {
    atomic_store(&fk_interrupt, 0);
    fk_error("interrupted");
  }
}

static V run(Lambda *L, V self, V *args) {
  FK_STACK_CHECK();
  check_interrupt();
  int cap = L->nslots > 8 ? L->nslots : 8;
  V fr[cap];
  if (L->n) memcpy(fr, args, (size_t)L->n * sizeof(V));
  Tail tc = {fr, cap, 0};
  for (;;) {
    V r = eval(L->body, fr, self, &tc);
    if (r) return r;
    check_interrupt(); /* a tail call: loops run here */
    self = tc.f;
    L = FK_INFO(self);
  }
}

void interp_install(Chunk *c) {
  if (CS->ffi_exports.n > ST->interp_install_nexports) {
    ST->interp_install_nexports = CS->ffi_exports.n;
    fk_error("foreign export `%s` needs compiled code: use funk build", CS->ffi_exports.a[CS->ffi_exports.n - 1]->name);
  }
  if ((size_t)CS->globals.n > ST->capgvals) {
    size_t ncap = ST->capgvals ? ST->capgvals : 256;
    while (ncap < (size_t)CS->globals.n) ncap *= 2;
    ST->gvals = xrealloc(ST->gvals, ncap * sizeof(V));
    memset(ST->gvals + ST->capgvals, 0, (ncap - ST->capgvals) * sizeof(V));
    if (!ST->capgvals) fk_add_roots(&ST->gvals, &ST->ngvals);
    ST->capgvals = ncap;
  }
  VEC(ForeignDecl *) fs = {0};
  for (size_t i = ST->ngvals; i < (size_t)CS->globals.n; i++) {
    if (CS->globals.a[i].prim >= 0) ST->gvals[i] = fk_prim_value(CS->globals.a[i].prim);
    if (CS->globals.a[i].ffi) VPUSH(fs, CS->globals.a[i].ffi);
  }
  ST->ngvals = (size_t)CS->globals.n; /* if loading fails, the imports stay undefined */
  ffi_load(fs.a, fs.n);
  for (int i = 0; i < fs.n; i++) /* arity 0: a C value, read once */
    ST->gvals[fs.a[i]->gidx] = fs.a[i]->arity ? fk_closure(fs.a[i]->code, fs.a[i]->arity, 0, NULL) : fs.a[i]->code(0, NULL);
  /* Functions first, then dictionaries, which refer to them, then values,
     which may use dictionaries. Creating a closure runs no code. */
  for (int i = 0; i < c->nraw; i++)
    if (!c->raw[i]->init) ST->gvals[c->raw[i]->gidx] = alloc_closure(c->raw[i]->e->lam);
  for (int i = 0; i < c->ng; i++)
    for (int j = 0; j < c->groups[i].n; j++)
      if (!c->groups[i].b[j]->init) ST->gvals[c->groups[i].b[j]->gidx] = alloc_closure(c->groups[i].b[j]->e->lam);
  for (int i = 0; i < c->nraw; i++)
    if (c->raw[i]->init) ST->gvals[c->raw[i]->gidx] = run(c->raw[i]->init, 0, NULL);
  for (int i = 0; i < c->ng; i++)
    for (int j = 0; j < c->groups[i].n; j++)
      if (c->groups[i].b[j]->init) ST->gvals[c->groups[i].b[j]->gidx] = run(c->groups[i].b[j]->init, 0, NULL);
}
