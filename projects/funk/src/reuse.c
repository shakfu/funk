/* reuse.c -- reuse of list cells, after dictionary passing (elab.h).

   A call passing a fresh list at a consuming parameter calls a copy,
   `f@reuseK`, which may write its output cells into the input's
   (reuse.md). In the copy, case scrutinees holding the input's spine are
   marked `own`; cgen reuses a cell matched from one for a TRMC output
   cell. Both properties are greatest fixpoints over the chunk's
   functions. Matched cells rebuilt unchanged are replaced by the
   scrutinee first, independently. */
#include "elab.h"

static bool pat_binds(Pat *p, const char *x) { return el_in(el_pat_nm(p, NULL), x); }

static Nm *group_nm(Group *g, Nm *s) {
  for (int i = 0; i < g->n; i++) s = el_nm(s, g->b[i]->name);
  return s;
}

/* Occurrences of the variable x, counting shadowed ones too. */
static int occ(Expr *e, const char *x);

static int occ_group(Group *g, const char *x) {
  int n = 0;
  for (int i = 0; i < g->n; i++) n += occ(g->b[i]->e, x);
  return n;
}

static int occ_rhs(Rhs *r, const char *x) {
  int n = 0;
  for (int i = 0; i < r->nwg; i++) n += occ_group(&r->wg[i], x);
  for (int i = 0; i < r->ng; i++) n += (r->guards[i] ? occ(r->guards[i], x) : 0) + occ(r->bodies[i], x);
  return n;
}

static int occ(Expr *e, const char *x) {
  int n = 0;
  switch (e->k) {
  case E_VAR: return e->name == x && !e->global_ref;
  case E_APP:
    n = occ(e->fn, x);
    for (int i = 0; i < e->n; i++) n += occ(e->args[i], x);
    return n;
  case E_LAM: return occ(e->lam->body, x);
  case E_LET: return occ_group(&e->g, x) + occ(e->body, x);
  case E_IF: return occ(e->cond, x) + occ(e->then_, x) + occ(e->else_, x);
  case E_CASE:
    for (int i = 0; i < e->ns; i++) n += occ(e->scruts[i], x);
    for (int a = 0; a < e->na; a++) n += occ_rhs(&e->alts[a]->rhs, x);
    return n;
  case E_ANN: return occ(e->body, x);
  default: return 0;
  }
}

/* Bit w: a constructor of t's type takes w words (element-reuse.md,
   phase 1); 0 for a type without cells, such as Int or a function. */
static uint32_t cell_sizes(Type *t) {
  t = t ? repr(t) : NULL;
  if (!t || t->k != TY_CON || !strcmp(t->name, "->")) return 0;
  uint32_t m = 0;
  for (int i = 0; i < CS->condefs.n; i++) {
    int a = CS->condefs.a[i].arity;
    if (a > 0 && a < 31 && CS->tydefs.a[CS->condefs.a[i].tydef].name == t->name) m |= 1u << (a + 1); /* a header word, then the fields */
  }
  return m;
}

/* The sizes of the cells e allocates directly, outside nested lambdas. */
static uint32_t alloc_sizes(Expr *e);

static uint32_t alloc_sizes_rhs(Rhs *r) {
  uint32_t m = 0;
  for (int i = 0; i < r->nwg; i++)
    for (int j = 0; j < r->wg[i].n; j++) m |= alloc_sizes(r->wg[i].b[j]->e);
  for (int i = 0; i < r->ng; i++) m |= (r->guards[i] ? alloc_sizes(r->guards[i]) : 0) | alloc_sizes(r->bodies[i]);
  return m;
}

static uint32_t alloc_sizes(Expr *e) {
  uint32_t m = 0;
  switch (e->k) {
  case E_APP:
    if (e->fn->k == E_CON && e->n == fk_cons[e->fn->idx].arity && e->n > 0 && e->n < 31) m |= 1u << (e->n + 1);
    m |= alloc_sizes(e->fn);
    for (int i = 0; i < e->n; i++) m |= alloc_sizes(e->args[i]);
    return m;
  case E_LET:
    for (int i = 0; i < e->g.n; i++) m |= alloc_sizes(e->g.b[i]->e);
    return m | alloc_sizes(e->body);
  case E_IF: return alloc_sizes(e->cond) | alloc_sizes(e->then_) | alloc_sizes(e->else_);
  case E_CASE:
    for (int i = 0; i < e->ns; i++) m |= alloc_sizes(e->scruts[i]);
    for (int a = 0; a < e->na; a++) m |= alloc_sizes_rhs(&e->alts[a]->rhs);
    return m;
  case E_ANN: return alloc_sizes(e->body);
  default: return 0;
  }
}

/* ------------------------------------------- ownership (element-reuse.md)

   A value is unowned, owned in its spine (a list's cells, another type's
   top cell), or unique: no cell reachable from it is referenced from
   anywhere else. A variable used at most once on each path is linear; a
   linear variable bound to an owned value owns it. A copy f@reuseK or
   f@ownK takes parameter K in the spine or unique mode; in it, a case on
   an owned value marks the scrutinee `own`, and cgen reuses the matched
   cell for an allocation of its size. Results are summarized per version,
   a function and its parameters' modes, as a greatest fixpoint. */

enum { M_NONE, M_SPINE, M_UNIQUE };

typedef struct Own { const char *name; int mode; struct Own *next; } Own;

enum { OWN_BLOCK = 1024 };

/* reuse_cells makes Own environments in fixpoint loops, about 30 KB an input;
   none outlives it, so they come from blocks freed at its end. */
static Own *own_push(Own *o, const char *name, int mode) {
  if (!ST->own_left) {
    VPUSH(ST->own_blocks, xmalloc(OWN_BLOCK * sizeof(Own)));
    ST->own_left = OWN_BLOCK;
  }
  Own *n = (Own *)ST->own_blocks.a[ST->own_blocks.n - 1] + OWN_BLOCK - ST->own_left--;
  *n = (Own){name, mode, o};
  return n;
}

void reuse_free(void) {
  for (int i = 0; i < ST->own_blocks.n; i++) xfree(ST->own_blocks.a[i]);
  ST->own_blocks.n = 0;
  ST->own_left = 0;
}

static int own_get(Own *o, const char *x) { /* -1: not a local */
  for (; o; o = o->next)
    if (o->name == x) return o->mode;
  return -1;
}

static int mode_min(int a, int b) { return a < b ? a : b; }

/* The top-level function f calls, unless a local shadows it. */
static Bind *own_callee(Expr *f, Own *o) {
  if (f->k != E_VAR || (!f->global_ref && own_get(o, f->name) >= 0)) return NULL;
  int g = global_lookup(f->name);
  Bind *b = g >= 0 ? CS->globals.a[g].bind : NULL;
  return b && b->e->k == E_LAM && b->e->lam->ptypes ? b : NULL;
}

/* Uses of x on the worst path through e; a use inside a lambda counts
   twice, since the lambda may run more than once. */
static int uses(Expr *e, const char *x);

static int uses_rhs(Rhs *r, const char *x) {
  int n = 0, m = 0;
  for (int i = 0; i < r->nwg; i++) n += occ_group(&r->wg[i], x);
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) n += uses(r->guards[i], x);
    int b = uses(r->bodies[i], x);
    if (b > m) m = b;
  }
  return n + m;
}

static int uses(Expr *e, const char *x) {
  int n = 0, m = 0;
  switch (e->k) {
  case E_VAR: return e->name == x && !e->global_ref;
  case E_APP:
    n = uses(e->fn, x);
    for (int i = 0; i < e->n; i++) n += uses(e->args[i], x);
    return n;
  case E_LAM: return occ(e->lam->body, x) ? 2 : 0;
  case E_LET: return occ_group(&e->g, x) + uses(e->body, x);
  case E_IF: {
    int t = uses(e->then_, x), f = uses(e->else_, x);
    return uses(e->cond, x) + (t > f ? t : f);
  }
  case E_CASE:
    for (int i = 0; i < e->ns; i++) n += uses(e->scruts[i], x);
    for (int a = 0; a < e->na; a++) {
      bool shadow = false;
      for (int i = 0; i < e->alts[a]->np; i++) shadow |= pat_binds(e->alts[a]->pats[i], x);
      int u = shadow ? 0 : uses_rhs(&e->alts[a]->rhs, x);
      if (u > m) m = u;
    }
    return n + m;
  case E_ANN: return uses(e->body, x);
  default: return 0;
  }
}

/* Cell sizes of t, its type arguments (list elements, tuple components)
   and its constructors' monomorphic fields: what an owned t can reuse. */
static uint32_t deep_sizes(Type *t) {
  t = t ? repr(t) : NULL;
  uint32_t m = cell_sizes(t);
  if (!m) return 0;
  for (int i = 0; i < t->n; i++) m |= cell_sizes(t->args[i]);
  for (int i = 0; i < CS->condefs.n; i++)
    if (CS->tydefs.a[CS->condefs.a[i].tydef].name == t->name)
      for (Type *f = CS->condefs.a[i].scheme ? repr(CS->condefs.a[i].scheme) : NULL; f && f->k == TY_CON && !strcmp(f->name, "->");
           f = repr(f->args[1]))
        m |= cell_sizes(f->args[0]);
  return m;
}

static int ver_find(Bind *f, uint32_t key) {
  for (int i = 0; i < ST->vers.n; i++)
    if (ST->vers.a[i].f == f && ST->vers.a[i].key == key) return i;
  return -1;
}

static int ver_res(Bind *f, uint32_t key) {
  int i = ver_find(f, key);
  if (i >= 0) return ST->vers.a[i].res;
  if (ST->vers_done) return M_NONE;
  VPUSH(ST->vers, ((Ver){f, key, M_UNIQUE, NULL})); /* optimistic, lowered by the fixpoint */
  ST->vers_new = true;
  return M_UNIQUE;
}

static int mode_of(Expr *e, Own *o);

/* The modes a call passes: an owned argument at a linear parameter of a
   function whose copies pay. */
static uint32_t arg_key(Bind *b, Expr *e, Own *o) {
  uint32_t key = 0;
  for (int j = 0; b->gain && j < e->n && j < 16; j++)
    if (b->linear >> j & 1) key |= (uint32_t)mode_of(e->args[j], o) << (2 * j);
  return key;
}

/* Is t known to hold no cell that reuse could take: a function (closures
   are never matched), or a type whose constructors take nothing (Int,
   Bool)? A type variable may stand for a list. */
static bool cell_free(Type *t) {
  t = t ? repr(t) : NULL;
  return t && t->k == TY_CON && (!strcmp(t->name, "->") || !cell_sizes(t));
}

/* A field: unique, or of a type without cells. */
static bool field_ok(Expr *a, Own *o) { return cell_free(a->ty) || mode_of(a, o) == M_UNIQUE; }

/* Pattern variables' modes, the scrutinee's in mode sm; a variable owns
   only if linear in r. */
static Own *pat_env(Pat *p, int sm, Rhs *r, Own *o) {
  switch (p->k) {
  case P_VAR: return own_push(o, p->name, uses_rhs(r, p->name) <= 1 ? sm : M_NONE);
  case P_CON:
    if (p->con == FK_C_CONS && p->n == 2) { /* the tail continues the spine; the head is owned if the list is unique */
      o = pat_env(p->args[0], sm == M_UNIQUE ? M_UNIQUE : M_NONE, r, o);
      return pat_env(p->args[1], sm, r, o);
    }
    for (int j = 0; j < p->n; j++) o = pat_env(p->args[j], sm == M_UNIQUE ? M_UNIQUE : M_NONE, r, o);
    return o;
  default: {
    for (Nm *n = el_pat_nm(p, NULL); n; n = n->next) o = own_push(o, n->name, M_NONE);
    return o;
  }
  }
}

static Own *alt_env(Expr *e, Alt *alt, Own *o) {
  Own *t = o;
  for (int i = 0; i < alt->np; i++) t = pat_env(alt->pats[i], mode_of(e->scruts[i], o), &alt->rhs, t);
  for (int i = 0; i < alt->rhs.nwg; i++) /* where values: lazy, so held by a thunk */
    for (int j = 0; j < alt->rhs.wg[i].n; j++) t = own_push(t, alt->rhs.wg[i].b[j]->name, M_NONE);
  return t;
}

static int mode_of(Expr *e, Own *o) {
  switch (e->k) {
  case E_INT: case E_FLOAT: case E_CHAR: case E_STR: return M_UNIQUE; /* no cell to reuse */
  case E_CON: return M_UNIQUE; /* nullary: a static object, never matched for reuse */
  case E_ANN: return mode_of(e->body, o);
  case E_VAR: {
    int m = e->global_ref ? -1 : own_get(o, e->name);
    return m < 0 ? M_NONE : m; /* a global is shared */
  }
  case E_LET: { /* a let value may be a thunk, which keeps its value: not owned */
    Own *t = o;
    for (int i = 0; i < e->g.n; i++) t = own_push(t, e->g.b[i]->name, M_NONE);
    return mode_of(e->body, t);
  }
  case E_IF: return mode_min(mode_of(e->then_, o), mode_of(e->else_, o));
  case E_CASE: {
    int m = M_UNIQUE;
    for (int a = 0; a < e->na && m; a++) {
      Own *t = alt_env(e, e->alts[a], o);
      for (int i = 0; i < e->alts[a]->rhs.ng; i++) m = mode_min(m, mode_of(e->alts[a]->rhs.bodies[i], t));
    }
    return m;
  }
  case E_APP: {
    Expr *f = e->fn;
    if (f->k == E_CON) {
      if (e->n != fk_cons[f->idx].arity) return M_NONE; /* a closure */
      if (f->idx == FK_C_CONS) {
        int tl = mode_of(e->args[1], o);
        return tl == M_NONE ? M_NONE : tl == M_UNIQUE && field_ok(e->args[0], o) ? M_UNIQUE : M_SPINE;
      }
      for (int i = 0; i < e->n; i++)
        if (!field_ok(e->args[i], o)) return M_SPINE;
      return M_UNIQUE;
    }
    if (f->k == E_VAR && (f->global_ref || own_get(o, f->name) < 0) && !strcmp(f->name, "error")) return M_UNIQUE; /* never returns */
    Bind *b = own_callee(f, o);
    if (b && b->own_of) return ver_res(b->own_of, b->own_key); /* a call already redirected to a copy */
    return b && e->n == b->e->lam->n ? ver_res(b, arg_key(b, e, o)) : M_NONE;
  }
  default: return M_NONE;
  }
}

/* Does e call a function whose copies pay, outside lambdas? */
static bool calls_gain(Expr *e, Own *o);

static bool calls_gain_rhs(Rhs *r, Own *o) {
  for (int i = 0; i < r->nwg; i++)
    for (int j = 0; j < r->wg[i].n; j++)
      if (calls_gain(r->wg[i].b[j]->e, o)) return true;
  for (int i = 0; i < r->ng; i++)
    if ((r->guards[i] && calls_gain(r->guards[i], o)) || calls_gain(r->bodies[i], o)) return true;
  return false;
}

static bool calls_gain(Expr *e, Own *o) {
  switch (e->k) {
  case E_APP: {
    Bind *b = own_callee(e->fn, o);
    if (b && b->gain && e->n == b->e->lam->n) return true;
    if (calls_gain(e->fn, o)) return true;
    for (int i = 0; i < e->n; i++)
      if (calls_gain(e->args[i], o)) return true;
    return false;
  }
  case E_LET:
    for (int i = 0; i < e->g.n; i++) o = own_push(o, e->g.b[i]->name, M_NONE);
    for (int i = 0; i < e->g.n; i++)
      if (calls_gain(e->g.b[i]->e, o)) return true;
    return calls_gain(e->body, o);
  case E_IF: return calls_gain(e->cond, o) || calls_gain(e->then_, o) || calls_gain(e->else_, o);
  case E_CASE:
    for (int i = 0; i < e->ns; i++)
      if (calls_gain(e->scruts[i], o)) return true;
    for (int a = 0; a < e->na; a++) {
      Own *t = o;
      for (int i = 0; i < e->alts[a]->np; i++)
        for (Nm *n = el_pat_nm(e->alts[a]->pats[i], NULL); n; n = n->next) t = own_push(t, n->name, M_NONE);
      if (calls_gain_rhs(&e->alts[a]->rhs, t)) return true;
    }
    return false;
  case E_ANN: return calls_gain(e->body, o);
  default: return false;
  }
}

static Own *params_env(Bind *b, uint32_t key) {
  Lambda *L = b->e->lam;
  Own *o = NULL;
  for (int j = 0; j < L->n; j++) o = own_push(o, L->params[j], j < 16 && (b->linear >> j & 1) ? (int)(key >> (2 * j) & 3) : M_NONE);
  return o;
}


static const char *own_copy(Bind *b, uint32_t key) {
  int v = ver_find(b, key);
  if (v >= 0 && ST->vers.a[v].copy) return ST->vers.a[v].copy->name;
  SB name = {0};
  sb_puts(&name, b->name);
  for (int j = 0; j < 16; j++) {
    int m = key >> (2 * j) & 3;
    if (m) sb_printf(&name, "@%s%d", m == M_SPINE ? "reuse" : "own", j);
  }
  const char *nm = intern(name.s);
  int g = global_lookup(nm); /* made in an earlier chunk: the same function and modes */
  if (g >= 0 && CS->globals.a[g].bind && CS->globals.a[g].bind->own_of == b) {
    if (v >= 0) ST->vers.a[v].copy = CS->globals.a[g].bind;
    return nm;
  }
  Bind *c = NEW(Bind);
  *c = (Bind){.name = nm, .e = copy_expr(b->own_src), .line = b->line, .col = b->col, .type = b->type,
              .linear = b->linear, .gain = b->gain, .own_key = key, .own_of = b};
  c->e->lam->name = nm;
  c->gidx = global_add(nm, NULL, c->e->lam);
  CS->globals.a[c->gidx].bind = c;
  VPUSH(CS->specialized, c);
  VPUSH(ST->own_work, c);
  if (v >= 0) ST->vers.a[v].copy = c;
  return nm;
}

/* Redirect calls passing owned values to copies, and mark owned
   scrutinees; or, before vers_done, only record the versions met. */
static void own_rw(Expr *e, Own *o);

static void own_rw_rhs(Rhs *r, Own *o) {
  for (int i = 0; i < r->nwg; i++)
    for (int j = 0; j < r->wg[i].n; j++) own_rw(r->wg[i].b[j]->e, o);
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) own_rw(r->guards[i], o);
    own_rw(r->bodies[i], o);
  }
}

static Own *unowned(Own *o) { /* the same locals, none owned: inside a lambda */
  Own *n = NULL;
  for (; o; o = o->next) n = own_push(n, o->name, M_NONE);
  return n;
}

static void own_rw(Expr *e, Own *o) {
  switch (e->k) {
  case E_APP: {
    Bind *b = own_callee(e->fn, o);
    if (b && !b->own_of && e->n == b->e->lam->n) {
      uint32_t key = arg_key(b, e, o);
      if (key && !ST->vers_done) ver_res(b, key);
      else if (key && ver_find(b, key) >= 0) {
        Expr *f = el_var(own_copy(b, key), e->fn->line, e->fn->col);
        f->global_ref = true;
        f->ty = e->fn->ty;
        e->fn = f;
      }
    }
    own_rw(e->fn, o);
    for (int i = 0; i < e->n; i++) own_rw(e->args[i], o);
    return;
  }
  case E_LAM: {
    Own *t = unowned(o);
    for (int i = 0; i < e->lam->n; i++) t = own_push(t, e->lam->params[i], M_NONE);
    own_rw(e->lam->body, t);
    return;
  }
  case E_LET:
    for (int i = 0; i < e->g.n; i++) o = own_push(o, e->g.b[i]->name, M_NONE);
    for (int i = 0; i < e->g.n; i++) own_rw(e->g.b[i]->e, o);
    own_rw(e->body, o);
    return;
  case E_IF:
    own_rw(e->cond, o);
    own_rw(e->then_, o);
    own_rw(e->else_, o);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) {
      if (mode_of(e->scruts[i], o) != M_NONE && ST->vers_done) e->scruts[i]->own = true; /* its matched cell is dead */
      own_rw(e->scruts[i], o);
    }
    for (int a = 0; a < e->na; a++) own_rw_rhs(&e->alts[a]->rhs, alt_env(e, e->alts[a], o));
    return;
  case E_ANN: own_rw(e->body, o); return;
  default: return;
  }
}

/* In an alternative matching s against C x1 .. xn, C x1 .. xn is s. */
static void recon_in(Expr *e, Expr *s, Pat *p);

static bool recon_binds(Nm *names, Expr *s, Pat *p) {
  for (; names; names = names->next) {
    if (names->name == s->name) return true;
    for (int i = 0; i < p->n; i++)
      if (names->name == p->args[i]->name) return true;
  }
  return false;
}

static void recon_rhs(Rhs *r, Expr *s, Pat *p) {
  for (int i = 0; i < r->nwg; i++)
    if (recon_binds(group_nm(&r->wg[i], NULL), s, p)) return;
  for (int i = 0; i < r->nwg; i++)
    for (int j = 0; j < r->wg[i].n; j++) recon_in(r->wg[i].b[j]->e, s, p);
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) recon_in(r->guards[i], s, p);
    recon_in(r->bodies[i], s, p);
  }
}

static void recon_in(Expr *e, Expr *s, Pat *p) {
  switch (e->k) {
  case E_APP: {
    bool same = e->fn->k == E_CON && e->fn->idx == p->con && e->n == p->n;
    for (int i = 0; same && i < e->n; i++)
      same = e->args[i]->k == E_VAR && !e->args[i]->global_ref && e->args[i]->name == p->args[i]->name;
    if (same) {
      Type *t = e->ty;
      *e = *copy_expr(s);
      e->ty = t;
      return;
    }
    recon_in(e->fn, s, p);
    for (int i = 0; i < e->n; i++) recon_in(e->args[i], s, p);
    return;
  }
  case E_LAM: {
    Nm *ps = NULL;
    for (int i = 0; i < e->lam->n; i++) ps = el_nm(ps, e->lam->params[i]);
    if (!recon_binds(ps, s, p)) recon_in(e->lam->body, s, p);
    return;
  }
  case E_LET:
    if (recon_binds(group_nm(&e->g, NULL), s, p)) return;
    for (int i = 0; i < e->g.n; i++) recon_in(e->g.b[i]->e, s, p);
    recon_in(e->body, s, p);
    return;
  case E_IF:
    recon_in(e->cond, s, p);
    recon_in(e->then_, s, p);
    recon_in(e->else_, s, p);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) recon_in(e->scruts[i], s, p);
    for (int a = 0; a < e->na; a++) {
      Nm *names = NULL;
      for (int i = 0; i < e->alts[a]->np; i++) names = el_pat_nm(e->alts[a]->pats[i], names);
      if (!recon_binds(names, s, p)) recon_rhs(&e->alts[a]->rhs, s, p);
    }
    return;
  case E_ANN: recon_in(e->body, s, p); return;
  default: return;
  }
}

static void recon(Expr *e);

static void recon_all_rhs(Rhs *r) {
  for (int i = 0; i < r->nwg; i++)
    for (int j = 0; j < r->wg[i].n; j++) recon(r->wg[i].b[j]->e);
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) recon(r->guards[i]);
    recon(r->bodies[i]);
  }
}

static void recon(Expr *e) {
  switch (e->k) {
  case E_APP:
    recon(e->fn);
    for (int i = 0; i < e->n; i++) recon(e->args[i]);
    return;
  case E_LAM: recon(e->lam->body); return;
  case E_LET:
    for (int i = 0; i < e->g.n; i++) recon(e->g.b[i]->e);
    recon(e->body);
    return;
  case E_IF:
    recon(e->cond);
    recon(e->then_);
    recon(e->else_);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) recon(e->scruts[i]);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      for (int i = 0; i < e->ns; i++) {
        Expr *s = e->scruts[i];
        Pat *p = alt->pats[i];
        if (s->k != E_VAR || p->k != P_CON || !p->n || pat_binds(p, s->name)) continue;
        bool vars = true;
        for (int j = 0; j < p->n; j++) vars &= p->args[j]->k == P_VAR;
        Nm *others = NULL;
        for (int j = 0; j < alt->np; j++)
          if (j != i) others = el_pat_nm(alt->pats[j], others);
        if (vars && !el_in(others, s->name)) recon_rhs(&alt->rhs, s, p);
      }
      recon_all_rhs(&alt->rhs);
    }
    return;
  case E_ANN: recon(e->body); return;
  default: return;
  }
}

void reuse_cells(Bind **bs, int n, Bind **cs, int nc) {
  VEC(Bind *) all = {0}, fns = {0};
  for (int i = 0; i < n + nc; i++) VPUSH(all, i < n ? bs[i] : cs[i - n]);
  ST->vers.n = 0;
  ST->vers_done = false;
  for (int i = 0; i < all.n; i++) {
    Bind *b = all.a[i];
    if (b->e->k == E_LAM && !CS->globals.a[b->gidx].bind) CS->globals.a[b->gidx].bind = b; /* type-class clones */
    recon(b->e);
    if (b->e->k != E_LAM || !b->e->lam->ptypes) continue;
    Lambda *L = b->e->lam;
    b->linear = 0;
    uint32_t sizes = alloc_sizes(L->body), wants = 0;
    for (int j = 0; j < L->n && j < 16; j++)
      if (cell_sizes(L->ptypes[j]) && uses(L->body, L->params[j]) <= 1) {
        b->linear |= 1u << j;
        wants |= deep_sizes(L->ptypes[j]);
      }
    b->gain = b->linear && (sizes & wants); /* it allocates what an owned parameter could give */
    b->own_src = copy_expr(b->e);
    VPUSH(fns, b);
  }
  for (bool changed = true; changed;) { /* copies also pay if they pass owned values to copies that do */
    changed = false;
    for (int i = 0; i < fns.n; i++) {
      Bind *b = fns.a[i];
      if (b->gain || !b->linear || !calls_gain(b->e->lam->body, params_env(b, 0))) continue;
      b->gain = changed = true;
    }
  }
  for (int i = 0; i < fns.n; i++) ver_res(fns.a[i], 0);
  for (bool changed = true; changed;) { /* lower each version's result to what its body gives */
    changed = ST->vers_new = false;
    for (int i = 0; i < ST->vers.n; i++) {
      Bind *b = ST->vers.a[i].f;
      Expr *src = b->own_src ? b->own_src : b->e; /* calls still name the originals */
      Own *o = params_env(b, ST->vers.a[i].key);
      int r = mode_of(src->lam->body, o);
      own_rw(src->lam->body, o); /* the versions its calls need, in every position */
      if (r < ST->vers.a[i].res) {
        ST->vers.a[i].res = r;
        changed = true;
      }
    }
    for (int i = 0; i < all.n; i++) /* values, such as main */
      if (all.a[i]->e->k != E_LAM || !all.a[i]->e->lam->ptypes) own_rw(all.a[i]->e, NULL);
    changed |= ST->vers_new;
  }
  ST->vers_done = true;
  for (int i = 0; i < fns.n; i++) own_rw(fns.a[i]->e, NULL);
  for (int i = 0; i < all.n; i++)
    if (all.a[i]->e->k != E_LAM || !all.a[i]->e->lam->ptypes) own_rw(all.a[i]->e, NULL);
  while (ST->own_work.n) {
    Bind *c = ST->own_work.a[--ST->own_work.n];
    own_rw(c->e->lam->body, params_env(c->own_of, c->own_key));
  }
  reuse_free();
  xfree(all.a);
  xfree(fns.a);
}
