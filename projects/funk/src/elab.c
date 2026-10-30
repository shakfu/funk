/* elab.c -- dictionary passing, after inference and defaulting.

   A binding whose scheme has constraints gets one dictionary parameter
   per constraint, in the order of preds_of. A use of an overloaded name
   gets the dictionaries for its instantiation (recorded by check.c on the
   E_VAR) as leading arguments. A dictionary is either a parameter in
   scope, a superclass field of one, or an instance dictionary applied to
   the dictionaries its context needs. Prelude overloads used at Int, Float
   or Char become direct primitive calls instead, so monomorphic
   arithmetic and comparison cost what they did before type classes.
   Integer literals of a non-builtin or polymorphic type become
   `fromInt d n`.

   Specialization: a call whose dictionaries are all known calls a clone
   of the function elaborated at those types, `fib@Int`, made once. The
   clone starts from the function's body before elaboration (`orig`), with
   its type variables mapped to the concrete types (`tsub`), so primitives
   and further specializations apply inside it. A class method at a known
   instance calls the instance's method directly. */
#include "funk.h"

typedef struct DEnv { Type *var; int cls; const char *name; struct DEnv *next; } DEnv;
typedef struct Sc { const char *name; Bind *b; struct Sc *next; } Sc; /* b NULL: monomorphic */

typedef struct { Type *from, *to; } TS;

typedef struct Nm { const char *name; struct Nm *next; } Nm;

/* Local variables of a call site that an argument uses, in order. */
typedef struct { VEC(const char *) names; VEC(Type *) types; } Fv;

/* Versions: a function and its parameters' modes, 2 bits each. */
typedef struct { Bind *f; uint32_t key; int res; Bind *copy; } Ver;

/* elab.c's state, one per session (embedding.md, phase 2) */
typedef struct {
  int dcounter;
  TS *tsub; /* type variables of the function being specialized */
  int ntsub;
  int nho; /* copies made, bounded so argument growth terminates */
  Fv *key_fv;
  Nm *keep_locals; /* the copy's leading parameters, during replace */
  VEC(Bind *) ho_work;
  int nhparam;
  int nbeta;
  int nlift;
  VEC(Bind *) lifted;
  VEC(Ver) vers;
  bool vers_new; /* the fixpoint met a version it had not seen */
  bool vers_done; /* the fixpoint is over: an unseen version is unowned */
  VEC(Bind *) own_work;
} ElabState;
static const ElabState elab_init = {0};
#define ST ((ElabState *)fk_here()->cc[CC_ELAB])

void *elab_state_new(void) {
  ElabState *s = malloc(sizeof *s);
  if (!s) fk_fatal("out of memory");
  *s = elab_init;
  return s;
}


/* t with the variables of tsub replaced. */
static Type *apply(Type *t) {
  t = repr(t);
  if (t->k == TY_VAR) {
    for (int i = 0; i < ST->ntsub; i++)
      if (ST->tsub[i].from == t) return ST->tsub[i].to;
    return t;
  }
  if (!t->n) return t;
  Type **a = xmalloc((size_t)t->n * sizeof *a);
  bool same = true;
  for (int i = 0; i < t->n; i++) same &= (a[i] = apply(t->args[i])) == repr(t->args[i]);
  return same ? t : tcon_new(t->name, t->n, a);
}

/* Depth of a ground type, or -1 if it has variables. */
static int ground_depth(Type *t) {
  t = repr(t);
  if (t->k == TY_VAR) return -1;
  int d = 0;
  for (int i = 0; i < t->n; i++) {
    int k = ground_depth(t->args[i]);
    if (k < 0) return -1;
    if (k > d) d = k;
  }
  return d + 1;
}

/* ------------------------------------------------------------- copying */

static Pat *copy_pat(Pat *p) {
  Pat *c = NEW(Pat);
  *c = *p;
  if (p->n) {
    c->args = xmalloc((size_t)p->n * sizeof *c->args);
    for (int i = 0; i < p->n; i++) c->args[i] = copy_pat(p->args[i]);
  }
  return c;
}

static Bind *copy_bind(Bind *b) {
  Bind *c = NEW(Bind);
  *c = *b;
  c->e = copy_expr(b->e);
  return c;
}

static Group copy_group(Group g) {
  Bind **bs = xmalloc((size_t)g.n * sizeof *bs + 1);
  for (int i = 0; i < g.n; i++) bs[i] = copy_bind(g.b[i]);
  g.b = bs;
  return g;
}

static Expr **copy_exprs(Expr **es, int n) {
  Expr **c = xmalloc((size_t)n * sizeof *c + 1);
  for (int i = 0; i < n; i++) c[i] = es[i] ? copy_expr(es[i]) : NULL;
  return c;
}

Expr *copy_expr(Expr *e) {
  Expr *c = NEW(Expr);
  *c = *e;
  switch (e->k) {
  case E_APP:
    c->fn = copy_expr(e->fn);
    c->args = copy_exprs(e->args, e->n);
    break;
  case E_LAM:
    c->lam = NEW(Lambda);
    *c->lam = *e->lam;
    c->lam->body = copy_expr(e->lam->body);
    break;
  case E_LET:
    c->g = copy_group(e->g);
    c->body = copy_expr(e->body);
    break;
  case E_IF:
    c->cond = copy_expr(e->cond);
    c->then_ = copy_expr(e->then_);
    c->else_ = copy_expr(e->else_);
    break;
  case E_CASE:
    c->scruts = copy_exprs(e->scruts, e->ns);
    c->alts = xmalloc((size_t)e->na * sizeof *c->alts);
    for (int a = 0; a < e->na; a++) {
      Alt *o = e->alts[a], *n = NEW(Alt);
      *n = *o;
      n->pats = xmalloc((size_t)o->np * sizeof *n->pats + 1);
      for (int i = 0; i < o->np; i++) n->pats[i] = copy_pat(o->pats[i]);
      n->rhs.wg = xmalloc((size_t)o->rhs.nwg * sizeof *n->rhs.wg + 1);
      for (int i = 0; i < o->rhs.nwg; i++) n->rhs.wg[i] = copy_group(o->rhs.wg[i]);
      n->rhs.guards = copy_exprs(o->rhs.guards, o->rhs.ng);
      n->rhs.bodies = copy_exprs(o->rhs.bodies, o->rhs.ng);
      c->alts[a] = n;
    }
    break;
  case E_ANN: c->body = copy_expr(e->body); break;
  default: break;
  }
  return c;
}

static Expr *mk(int k, int line, int col) {
  Expr *e = NEW(Expr);
  e->k = k;
  e->line = line;
  e->col = col;
  return e;
}

/* Dictionaries and the functions building them have type dict_type. */
static Expr *var(const char *name, int line, int col) {
  Expr *e = mk(E_VAR, line, col);
  e->name = name;
  e->ty = CS->dict_type;
  return e;
}

static Expr *app(Expr *f, int n, Expr **args) {
  if (!n) return f;
  Expr *e = mk(E_APP, f->line, f->col);
  e->fn = f;
  e->n = n;
  e->args = args;
  e->ty = CS->dict_type;
  return e;
}

static DEnv *denv(DEnv *env, Type *v, int cls, const char *name) {
  DEnv *d = NEW(DEnv);
  *d = (DEnv){v, cls, name, env};
  return d;
}

static Sc *scope(Sc *sc, const char *name, Bind *b) {
  Sc *s = NEW(Sc);
  *s = (Sc){name, b, sc};
  return s;
}

/* Extract the dictionary for class `to` from e, a dictionary for `from`. */
static Expr *super_path(int from, int to, Expr *e, int line, int col) {
  if (from == to) return e;
  ClassDef *c = &CS->classes.a[from];
  for (int i = 0; i < c->nsup; i++) {
    int s = c->sups[i];
    if (!(CS->classes.a[s].closure & ((uint64_t)1 << to))) continue;
    Expr **a = xmalloc(sizeof *a);
    a[0] = e;
    const char *sel = intern(xsprintf("$sup.%s.%s", c->name, CS->classes.a[s].name));
    return super_path(s, to, app(var(sel, line, col), 1, a), line, col);
  }
  abort();
}

/* The constraints an instance's dictionary function takes, in order. */
static int inst_preds(InstDef *in, Pred **out) {
  VEC(Pred) ps = {0};
  for (int i = 0; i < in->arity; i++) {
    Pred *vp;
    int n = preds_of(in->vars[i], &vp); /* the variable alone: its minimal classes */
    for (int k = 0; k < n; k++) VPUSH(ps, vp[k]);
  }
  *out = ps.a;
  return ps.n;
}

static Expr *dict_for(int cls, Type *t, DEnv *env, int line, int col) {
  t = apply(t);
  if (t->k == TY_VAR) {
    for (DEnv *d = env; d; d = d->next)
      if (d->var == t && d->cls == cls) return var(d->name, line, col);
    for (DEnv *d = env; d; d = d->next)
      if (d->var == t && (CS->classes.a[d->cls].closure & ((uint64_t)1 << cls)))
        return super_path(d->cls, cls, var(d->name, line, col), line, col);
    cerror(line, col, "ambiguous type: no dictionary for `%s`; add a type signature", CS->classes.a[cls].name);
  }
  InstDef *in = find_inst(cls, t->name);
  if (!in) cerror(line, col, "no instance `%s %s`", CS->classes.a[cls].name, t->name);
  Pred *ps;
  int n = inst_preds(in, &ps);
  Expr **a = xmalloc((size_t)n * sizeof *a + 1);
  for (int i = 0; i < n; i++) {
    int k = 0;
    while (in->vars[k] != ps[i].t) k++;
    a[i] = dict_for(ps[i].cls, t->args[k], env, line, col);
  }
  return app(var(CS->globals.a[in->dict].name, line, col), n, a);
}

/* Types whose instances are primitives: primEqInt, primAppendString, ... */
static bool builtin_num(Type *t) {
  const char *n = t->name;
  return t->k == TY_CON && (!strcmp(n, "Int") || !strcmp(n, "Float") || !strcmp(n, "Char") || !strcmp(n, "String"));
}

static void elab(Expr *e, Sc *sc, DEnv *env);

/* Apply tsub to every type recorded in a clone, so its parameters and
   locals have the concrete types the typed backend needs. The arrays are
   replaced, not edited: copy_expr shares them with the original. */
static void subst_pat(Pat *p) {
  if (p->ty) p->ty = apply(p->ty);
  for (int i = 0; i < p->n; i++) subst_pat(p->args[i]);
}

static void subst_types(Expr *e);

static void subst_group(Group *g) {
  for (int i = 0; i < g->n; i++) {
    if (g->b[i]->type) g->b[i]->type = apply(g->b[i]->type);
    subst_types(g->b[i]->e);
  }
}

static void subst_types(Expr *e) {
  if (e->ty) e->ty = apply(e->ty);
  switch (e->k) {
  case E_APP:
    subst_types(e->fn);
    for (int i = 0; i < e->n; i++) subst_types(e->args[i]);
    return;
  case E_LAM: {
    Lambda *L = e->lam;
    if (L->ptypes) {
      Type **ts = xmalloc((size_t)L->n * sizeof *ts + 1);
      for (int i = 0; i < L->n; i++) ts[i] = L->ptypes[i] ? apply(L->ptypes[i]) : NULL;
      L->ptypes = ts;
    }
    subst_types(L->body);
    return;
  }
  case E_LET:
    subst_group(&e->g);
    subst_types(e->body);
    return;
  case E_IF:
    subst_types(e->cond);
    subst_types(e->then_);
    subst_types(e->else_);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) subst_types(e->scruts[i]);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      for (int i = 0; i < alt->np; i++) subst_pat(alt->pats[i]);
      for (int i = 0; i < alt->rhs.nwg; i++) subst_group(&alt->rhs.wg[i]);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) subst_types(alt->rhs.guards[i]);
        subst_types(alt->rhs.bodies[i]);
      }
    }
    return;
  case E_ANN: subst_types(e->body); return;
  default: return;
  }
}

/* Global name of g's clone at the types tys of its constraints, made
   now if new; NULL if not worth it (a type nested too deeply, as in
   polymorphic recursion). */
static const char *specialize(int g, Type **tys, int n) {
  Bind *ob = CS->globals.a[g].bind;
  Pred *ps = ob->preds; /* same order as the use's constraints */
  if (ob->npreds != n) return NULL;
  SB nm = {0};
  sb_printf(&nm, "%s@", ob->name);
  for (int i = 0; i < n; i++) {
    if (ground_depth(tys[i]) > 4) return NULL;
    sb_printf(&nm, "%s%s", i ? "," : "", type_str(tys[i]));
  }
  const char *name = intern(nm.s);
  if (global_lookup(name) >= 0) return name;
  TS *saved = ST->tsub;
  int nsaved = ST->ntsub;
  VEC(TS) ts = {0};
  for (int i = 0; i < n; i++) {
    VPUSH(ts, ((TS){ps[i].t, tys[i]}));
    for (int k = 0; k < ob->nskolem; k++)
      if (ob->skolem[2 * k] == ps[i].t) VPUSH(ts, ((TS){ob->skolem[2 * k + 1], tys[i]}));
  }
  ST->tsub = ts.a;
  ST->ntsub = ts.n;
  Bind *b = NEW(Bind);
  *b = (Bind){.name = name, .e = copy_expr(ob->orig), .line = ob->line, .col = ob->col, .from = ob->from ? ob->from : ob};
  b->e->lam->name = name;
  b->type = apply(CS->globals.a[g].type);
  subst_types(b->e);
  b->gidx = global_add(name, b->type, b->e->lam); /* before the body, for recursion */
  elab(b->e, NULL, NULL);
  ST->tsub = saved;
  ST->ntsub = nsaved;
  VPUSH(CS->specialized, b);
  return name;
}

/* Dictionary arguments for a use of variable v. v may be renamed instead
   to a primitive, an instance method or a specialization. */
static int var_dicts(Expr *v, Sc *sc, DEnv *env, Expr ***out) {
  int g = -1, n = 0;
  Pred *ps = NULL;
  Sc *s = sc;
  while (s && s->name != v->name) s = s->next;
  if (s) {
    if (s->b) {
      n = s->b->npreds;
      ps = s->b->preds;
    }
  } else if ((g = global_lookup(v->name)) >= 0) {
    n = preds_of(CS->globals.a[g].type, &ps);
  }
  if (!n) return 0;
  if (v->npreds == n) ps = v->preds; /* else a use inside its own group: the binding's own dictionaries */
  Type **tys = xmalloc((size_t)n * sizeof *tys);
  for (int i = 0; i < n; i++) tys[i] = apply(ps[i].t);
  if (g >= 0 && CS->globals.a[g].spec && n == 1 && builtin_num(tys[0])) {
    const char *p = intern(xsprintf("prim%s%s", CS->globals.a[g].spec, tys[0]->name));
    if (global_lookup(p) >= 0) {
      v->name = p;
      return 0;
    }
  }
  InstDef *in;
  if (g >= 0 && CS->globals.a[g].mcls >= 0 && tys[0]->k == TY_CON && (in = find_inst(CS->globals.a[g].mcls, tys[0]->name))) {
    ClassDef *c = &CS->classes.a[in->cls]; /* method at a known instance: $m.C.T.m and its context */
    const char *m = intern(xsprintf("$m.%s.%s.%s", c->name, in->tycon, c->meths[CS->globals.a[g].midx]));
    Type *t0 = tys[0];
    v->name = m;
    g = global_lookup(m);
    n = preds_of(CS->globals.a[g].type, &ps);
    tys = xmalloc((size_t)n * sizeof *tys + 1);
    for (int i = 0; i < n; i++) {
      int k = 0;
      while (in->vars[k] != ps[i].t) k++;
      tys[i] = apply(t0->args[k]);
    }
    if (!n) return 0;
  }
  bool ground = g >= 0 && CS->globals.a[g].bind && CS->globals.a[g].bind->orig;
  for (int i = 0; ground && i < n; i++) ground = ground_depth(tys[i]) > 0;
  const char *sp = ground ? specialize(g, tys, n) : NULL;
  if (sp) {
    v->name = sp;
    return 0;
  }
  Expr **a = xmalloc((size_t)n * sizeof *a);
  for (int i = 0; i < n; i++) a[i] = dict_for(ps[i].cls, tys[i], env, v->line, v->col);
  *out = a;
  return n;
}

static void elab_one(Bind *b, Sc *sc, DEnv *env) {
  Pred *ps = b->preds;
  int n = b->npreds;
  if (n) {
    const char **names = xmalloc((size_t)n * sizeof *names);
    for (int i = 0; i < n; i++) {
      names[i] = intern(xsprintf("$d%d", ST->dcounter++));
      env = denv(env, ps[i].t, ps[i].cls, names[i]);
      for (int k = 0; k < b->nskolem; k++) /* the body was checked against rigid copies */
        if (b->skolem[2 * k] == ps[i].t) env = denv(env, b->skolem[2 * k + 1], ps[i].cls, names[i]);
    }
    Type **dts = xmalloc((size_t)n * sizeof *dts);
    for (int i = 0; i < n; i++) dts[i] = CS->dict_type;
    if (b->e->k == E_LAM) {
      Lambda *L = b->e->lam;
      const char **all = xmalloc((size_t)(n + L->n) * sizeof *all);
      copy_n(all, names, (size_t)n * sizeof *all);
      copy_n(all + n, L->params, (size_t)L->n * sizeof *all);
      Type **ts = xmalloc((size_t)(n + L->n) * sizeof *ts);
      copy_n(ts, dts, (size_t)n * sizeof *ts);
      copy_n(ts + n, L->ptypes, (size_t)L->n * sizeof *ts);
      L->params = all;
      L->ptypes = ts;
      L->n += n;
    } else {
      Expr *body = b->e;
      Expr *l = mk(E_LAM, body->line, body->col);
      Lambda *L = NEW(Lambda);
      *L = (Lambda){.n = n, .params = names, .ptypes = dts, .body = body, .line = body->line, .col = body->col,
                    .name = b->name};
      l->lam = L;
      l->ty = CS->dict_type;
      b->e = l;
    }
  }
  elab(b->e, sc, env);
}

static Sc *elab_group(Group *g, Sc *sc, DEnv *env) {
  for (int i = 0; i < g->n; i++) sc = scope(sc, g->b[i]->name, g->b[i]);
  for (int i = 0; i < g->n; i++) elab_one(g->b[i], sc, env);
  return sc;
}

static Sc *pat_scope(Pat *p, Sc *sc) {
  if (p->k == P_VAR) return scope(sc, p->name, NULL);
  for (int i = 0; i < p->n; i++) sc = pat_scope(p->args[i], sc);
  return sc;
}

static void elab(Expr *e, Sc *sc, DEnv *env) {
  switch (e->k) {
  case E_VAR: {
    Expr **ds;
    int n = var_dicts(e, sc, env, &ds);
    if (!n) return;
    Expr *v = NEW(Expr);
    *v = *e;
    *e = *app(v, n, ds);
    e->ty = v->ty; /* the value's type; v itself now takes dictionaries first */
    return;
  }
  case E_INT: {
    Type *t = e->ty ? apply(e->ty) : NULL;
    if (t && t->k == TY_CON && (!strcmp(t->name, "Int") || !strcmp(t->name, "Float"))) {
      e->ty = t; /* a clone's literal: resolve.c makes Float literals from this */
      return;
    }
    if (!t) return;
    Expr *lit = mk(E_INT, e->line, e->col);
    lit->ival = e->ival;
    lit->ty = tcon_new(intern("Int"), 0, NULL);
    InstDef *in = t->k == TY_CON ? find_inst(CS->num_class, t->name) : NULL;
    int m = in && !in->arity ? global_lookup(intern(xsprintf("$m.Num.%s.fromInt", t->name))) : -1;
    if (m >= 0) { /* a known instance: its fromInt directly, without a dictionary */
      Expr **a = xmalloc(sizeof *a);
      a[0] = lit;
      *e = *app(var(CS->globals.a[m].name, e->line, e->col), 1, a);
      e->ty = t;
      e->fn->idx = m;
      e->fn->known = CS->globals.a[m].lam;
      return;
    }
    Expr **a = xmalloc(2 * sizeof *a);
    a[0] = dict_for(CS->num_class, t, env, e->line, e->col);
    a[1] = lit;
    *e = *app(var(intern("$fromInt"), e->line, e->col), 2, a);
    e->ty = t;
    return;
  }
  case E_APP: {
    if (e->fn->k == E_VAR) {
      Expr **ds;
      int n = var_dicts(e->fn, sc, env, &ds);
      if (n) { /* f d.. x.., one application so known calls stay saturated */
        Expr **all = xmalloc((size_t)(n + e->n) * sizeof *all);
        copy_n(all, ds, (size_t)n * sizeof *all);
        copy_n(all + n, e->args, (size_t)e->n * sizeof *all);
        e->args = all;
        e->n += n;
        for (int i = n; i < e->n; i++) elab(e->args[i], sc, env);
        return;
      }
    } else {
      elab(e->fn, sc, env);
    }
    for (int i = 0; i < e->n; i++) elab(e->args[i], sc, env);
    return;
  }
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) sc = scope(sc, e->lam->params[i], NULL);
    elab(e->lam->body, sc, env);
    return;
  case E_LET: elab(e->body, elab_group(&e->g, sc, env), env); return;
  case E_IF:
    elab(e->cond, sc, env);
    elab(e->then_, sc, env);
    elab(e->else_, sc, env);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) elab(e->scruts[i], sc, env);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Sc *s = sc;
      for (int i = 0; i < alt->np; i++) s = pat_scope(alt->pats[i], s);
      for (int i = 0; i < alt->rhs.nwg; i++) s = elab_group(&alt->rhs.wg[i], s, env);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) elab(alt->rhs.guards[i], s, env);
        elab(alt->rhs.bodies[i], s, env);
      }
    }
    return;
  case E_ANN: elab(e->body, sc, env); return;
  default: return;
  }
}

void elab_bind(Bind *b) { elab_one(b, NULL, NULL); }

/* $dict.C.T = [\c.. ->] $D.C $m.C.T.m1 .. superclass dictionaries */
Bind *dict_bind(InstDef *in) {
  ClassDef *c = &CS->classes.a[in->cls];
  int line = in->line, col = in->col;
  Pred *ps;
  int n = inst_preds(in, &ps);
  const char **params = xmalloc((size_t)n * sizeof *params + 1);
  DEnv *env = NULL;
  for (int i = 0; i < n; i++) {
    params[i] = intern(xsprintf("$c%d", i));
    env = denv(env, ps[i].t, ps[i].cls, params[i]);
  }
  Type *head = tcon_new(in->tycon, in->arity, in->vars);
  int nf = c->nmeth + c->nsup;
  Expr **fields = xmalloc((size_t)nf * sizeof *fields + 1);
  for (int i = 0; i < c->nmeth; i++) {
    const char *m = intern(xsprintf("$m.%s.%s.%s", c->name, in->tycon, c->meths[i]));
    Pred *mp;
    int k = preds_of(CS->globals.a[global_lookup(m)].type, &mp);
    Expr **a = xmalloc((size_t)k * sizeof *a + 1);
    for (int j = 0; j < k; j++) a[j] = dict_for(mp[j].cls, mp[j].t, env, line, col);
    fields[i] = app(var(m, line, col), k, a);
  }
  for (int i = 0; i < c->nsup; i++) fields[c->nmeth + i] = dict_for(c->sups[i], head, env, line, col);
  Expr *con = mk(E_CON, line, col);
  con->name = CS->condefs.a[c->con].name;
  con->idx = c->con;
  Expr *body = app(con, nf, fields);
  if (n) {
    Expr *l = mk(E_LAM, line, col);
    Lambda *L = NEW(Lambda);
    *L = (Lambda){.n = n, .params = params, .body = body, .line = line, .col = col};
    l->lam = L;
    body = l;
  }
  Bind *b = NEW(Bind);
  *b = (Bind){.name = CS->globals.a[in->dict].name, .e = body, .line = line, .col = col};
  if (body->k == E_LAM) body->lam->name = b->name;
  return b;
}

/* ------------------------------------- specialization on function arguments */

/* After elaboration, a call to a top-level function whose argument for a
   function-typed parameter is built from globals, literals, constructors
   and lambdas calls a copy of the function with that parameter replaced
   by the argument: `map (flip primMulInt 3) xs` calls
   `map@{flip primMulInt 3}`, where `f x` is a known call. Local variables
   of the call site in the argument (`\q -> q : qs`) become leading
   parameters of the copy, passed by the call. Copies are keyed by the
   argument's structure with those variables numbered, so a recursive call
   inside a copy, which passes the copy's own parameters, reuses it.
   Global references copied into a body are pinned, so a local of the same
   name there cannot capture them. */

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


static int fv_index(Fv *fv, const char *name) {
  for (int i = 0; i < fv->names.n; i++)
    if (fv->names.a[i] == name) return i;
  return -1;
}

/* Can e be substituted into a copy? Its variables must be globals, its
   own lambda parameters (inner), or locals of the call site (outer), which
   are collected in fv. */
static bool liftable(Expr *e, Nm *inner, Nm *outer, int depth, Fv *fv) {
  if (depth > 6) return false;
  switch (e->k) {
  case E_VAR:
    if (in(inner, e->name)) return true;
    if (!e->global_ref && in(outer, e->name)) {
      if (fv_index(fv, e->name) < 0) {
        VPUSH(fv->names, e->name);
        VPUSH(fv->types, e->ty);
      }
      return true;
    }
    return global_lookup(e->name) >= 0;
  case E_CON: case E_INT: case E_FLOAT: case E_CHAR: case E_STR: return true;
  case E_APP:
    if (!liftable(e->fn, inner, outer, depth + 1, fv)) return false;
    for (int i = 0; i < e->n; i++)
      if (!liftable(e->args[i], inner, outer, depth + 1, fv)) return false;
    return true;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) inner = nm(inner, e->lam->params[i]);
    return liftable(e->lam->body, inner, outer, depth + 1, fv);
  case E_IF:
    return liftable(e->cond, inner, outer, depth + 1, fv) && liftable(e->then_, inner, outer, depth + 1, fv) &&
           liftable(e->else_, inner, outer, depth + 1, fv);
  case E_ANN: return liftable(e->body, inner, outer, depth + 1, fv);
  case E_CASE: /* as clauses with patterns desugar: \$a0 $a1 -> case $a0 $a1 of ... */
    for (int i = 0; i < e->ns; i++)
      if (!liftable(e->scruts[i], inner, outer, depth + 1, fv)) return false;
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      if (alt->rhs.nwg) return false;
      Nm *t = inner;
      for (int i = 0; i < alt->np; i++) t = pat_nm(alt->pats[i], t);
      for (int i = 0; i < alt->rhs.ng; i++)
        if ((alt->rhs.guards[i] && !liftable(alt->rhs.guards[i], t, outer, depth + 1, fv)) ||
            !liftable(alt->rhs.bodies[i], t, outer, depth + 1, fv))
          return false;
    }
    return true;
  default: return false;
  }
}

static void pat_key(SB *b, Pat *p, Nm **params) {
  switch (p->k) {
  case P_VAR: *params = nm(*params, p->name); sb_puts(b, "v"); return;
  case P_WILD: sb_puts(b, "_"); return;
  case P_INT: sb_printf(b, "%lld", p->ival); return;
  case P_CHAR: sb_printf(b, "'%lld", p->ival); return;
  case P_STR: sb_printf(b, "\"%lld:%s", p->ival, p->name); return;
  default:
    sb_printf(b, "(%s", p->name);
    for (int i = 0; i < p->n; i++) {
      sb_puts(b, " ");
      pat_key(b, p->args[i], params);
    }
    sb_puts(b, ")");
  }
}

/* The structure of an argument, as a key; lambda parameters and the call
   site's locals (fv) are numbered, so equal structures give equal keys. */
static void key(SB *b, Expr *e, Nm *params) {
  switch (e->k) {
  case E_VAR: {
    int i = 0;
    for (Nm *s = params; s; s = s->next, i++)
      if (s->name == e->name) {
        sb_printf(b, "#%d", i);
        return;
      }
    if (!e->global_ref && (i = fv_index(ST->key_fv, e->name)) >= 0) {
      sb_printf(b, "$%d", i);
      return;
    }
    sb_puts(b, e->name);
    return;
  }
  case E_CON: sb_puts(b, e->name); return;
  case E_INT: sb_printf(b, "%lld", e->ival); return;
  case E_FLOAT: sb_printf(b, "%.17g", e->fval); return;
  case E_CHAR: sb_printf(b, "'%lld", e->ival); return;
  case E_STR: sb_printf(b, "\"%d:%s", e->slen, e->name); return;
  case E_APP:
    sb_puts(b, "(");
    key(b, e->fn, params);
    for (int i = 0; i < e->n; i++) {
      sb_puts(b, " ");
      key(b, e->args[i], params);
    }
    sb_puts(b, ")");
    return;
  case E_LAM:
    sb_printf(b, "(\\%d ", e->lam->n);
    for (int i = 0; i < e->lam->n; i++) params = nm(params, e->lam->params[i]);
    key(b, e->lam->body, params);
    sb_puts(b, ")");
    return;
  case E_IF:
    sb_puts(b, "(if ");
    key(b, e->cond, params);
    sb_puts(b, " ");
    key(b, e->then_, params);
    sb_puts(b, " ");
    key(b, e->else_, params);
    sb_puts(b, ")");
    return;
  case E_ANN: key(b, e->body, params); return;
  case E_CASE:
    sb_puts(b, "(case");
    for (int i = 0; i < e->ns; i++) {
      sb_puts(b, " ");
      key(b, e->scruts[i], params);
    }
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *ps = params;
      sb_puts(b, " |");
      for (int i = 0; i < alt->np; i++) {
        sb_puts(b, " ");
        pat_key(b, alt->pats[i], &ps);
      }
      for (int i = 0; i < alt->rhs.ng; i++) {
        sb_puts(b, " ->");
        if (alt->rhs.guards[i]) {
          sb_puts(b, " ? ");
          key(b, alt->rhs.guards[i], ps);
        }
        sb_puts(b, " ");
        key(b, alt->rhs.bodies[i], ps);
      }
    }
    sb_puts(b, ")");
    return;
  default: abort();
  }
}

/* Pin the global references of a copied argument. Variables in inner (its
   lambda parameters, and the copy's leading parameters) stay local. */
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

/* In a copy of an argument, rename the call site's locals to the copy's
   leading parameters. */
static void rename_fv(Expr *e, Fv *fv, const char **to, Nm *inner) {
  switch (e->k) {
  case E_VAR: {
    int i;
    if (!in(inner, e->name) && !e->global_ref && (i = fv_index(fv, e->name)) >= 0) e->name = to[i];
    return;
  }
  case E_APP:
    rename_fv(e->fn, fv, to, inner);
    for (int i = 0; i < e->n; i++) rename_fv(e->args[i], fv, to, inner);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) inner = nm(inner, e->lam->params[i]);
    rename_fv(e->lam->body, fv, to, inner);
    return;
  case E_IF:
    rename_fv(e->cond, fv, to, inner);
    rename_fv(e->then_, fv, to, inner);
    rename_fv(e->else_, fv, to, inner);
    return;
  case E_ANN: rename_fv(e->body, fv, to, inner); return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) rename_fv(e->scruts[i], fv, to, inner);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = inner;
      for (int i = 0; i < alt->np; i++) t = pat_nm(alt->pats[i], t);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) rename_fv(alt->rhs.guards[i], fv, to, t);
        rename_fv(alt->rhs.bodies[i], fv, to, t);
      }
    }
    return;
  default: return;
  }
}


/* Replace free occurrences of variable x in e by copies of arg. */
static void replace(Expr *e, const char *x, Expr *arg);
static void replace_alt(Alt *alt, const char *x, Expr *arg);

static void replace_group(Group *g, const char *x, Expr *arg) {
  for (int i = 0; i < g->n; i++) replace(g->b[i]->e, x, arg);
}

static bool binds_in(Group *g, const char *x) {
  for (int i = 0; i < g->n; i++)
    if (g->b[i]->name == x) return true;
  return false;
}

static void replace(Expr *e, const char *x, Expr *arg) {
  switch (e->k) {
  case E_VAR:
    if (e->name == x && !e->global_ref) {
      Expr *c = copy_expr(arg);
      pin(c, ST->keep_locals);
      *e = *c;
    }
    return;
  case E_APP:
    replace(e->fn, x, arg);
    for (int i = 0; i < e->n; i++) replace(e->args[i], x, arg);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++)
      if (e->lam->params[i] == x) return;
    replace(e->lam->body, x, arg);
    return;
  case E_LET:
    if (binds_in(&e->g, x)) return;
    replace_group(&e->g, x, arg);
    replace(e->body, x, arg);
    return;
  case E_IF:
    replace(e->cond, x, arg);
    replace(e->then_, x, arg);
    replace(e->else_, x, arg);
    return;
  case E_CASE:
    /* `case x of v -> ...`, as clauses desugar: v is the argument too, and
       the scrutinee goes when every alternative only binds or ignores it. */
    for (int i = 0; i < e->ns; i++) {
      if (e->scruts[i]->k != E_VAR || e->scruts[i]->name != x || e->scruts[i]->global_ref) continue;
      bool simple = true;
      for (int a = 0; a < e->na; a++) {
        Pat *p = e->alts[a]->pats[i];
        if (p->k == P_VAR) {
          p->k = P_WILD; /* first, so replace_alt does not see it as shadowing */
          replace_alt(e->alts[a], p->name, arg);
        }
        simple &= p->k == P_WILD;
      }
      if (!simple) continue;
      for (int a = 0; a < e->na; a++) {
        Alt *alt = e->alts[a];
        move_n(alt->pats + i, alt->pats + i + 1, (size_t)(alt->np - i - 1) * sizeof *alt->pats);
        alt->np--;
      }
      move_n(e->scruts + i, e->scruts + i + 1, (size_t)(e->ns - i - 1) * sizeof *e->scruts);
      e->ns--;
      i--;
    }
    for (int i = 0; i < e->ns; i++) replace(e->scruts[i], x, arg);
    for (int a = 0; a < e->na; a++) replace_alt(e->alts[a], x, arg);
    return;
  case E_ANN: replace(e->body, x, arg); return;
  default: return;
  }
}

/* replace within one alternative, unless it binds x. */
static void replace_alt(Alt *alt, const char *x, Expr *arg) {
  for (int i = 0; i < alt->np; i++)
    if (in(pat_nm(alt->pats[i], NULL), x)) return;
  for (int i = 0; i < alt->rhs.nwg; i++)
    if (binds_in(&alt->rhs.wg[i], x)) return;
  for (int i = 0; i < alt->rhs.nwg; i++) replace_group(&alt->rhs.wg[i], x, arg);
  for (int i = 0; i < alt->rhs.ng; i++) {
    if (alt->rhs.guards[i]) replace(alt->rhs.guards[i], x, arg);
    replace(alt->rhs.bodies[i], x, arg);
  }
}

static bool fun_type(Type *t) {
  t = t ? repr(t) : NULL;
  return t && t->k == TY_CON && !strcmp(t->name, "->");
}


/* Bind the variables of the callee's type pat to the call site's ground
   type t, so a copy's parameters get C types (typed-backend.md). */
typedef VEC(TS) TSs;

static void match_type(Type *pat, Type *t, TSs *out) {
  pat = repr(pat);
  t = repr(t);
  if (pat->k == TY_VAR) {
    for (int i = 0; i < out->n; i++)
      if (out->a[i].from == pat) return;
    if (ground_depth(t) >= 0 && ground_depth(t) <= 4) VPUSH(*out, ((TS){pat, t}));
    return;
  }
  if (t->k != TY_CON || pat->name != t->name || pat->n != t->n) return;
  for (int i = 0; i < pat->n; i++) match_type(pat->args[i], t->args[i], out);
}

/* Specialize the call e if it qualifies; locals are the call site's. */
static void ho_call(Expr *e, Nm *locals) {
  Expr *f = e->fn;
  if (f->k != E_VAR || (!f->global_ref && in(locals, f->name))) return;
  int g = global_lookup(f->name);
  Bind *fb = g >= 0 ? CS->globals.a[g].bind : NULL;
  if (!fb || fb->e->k != E_LAM || !fb->e->lam->ptypes || ST->nho >= 4000) return;
  Lambda *L = fb->e->lam;
  int m = e->n < L->n ? e->n : L->n;
  bool *use = xcalloc((size_t)m + 1, sizeof *use);
  Fv fv = {0};
  int nuse = 0;
  for (int i = 0; i < m; i++) {
    Expr *x = e->args[i];
    if (!fun_type(L->ptypes[i]) || (x->k == E_VAR && !x->global_ref && in(locals, x->name))) continue;
    Fv try = fv;
    try.names.a = xmalloc((size_t)(fv.names.n + 8) * sizeof(char *));
    try.types.a = xmalloc((size_t)(fv.names.n + 8) * sizeof(Type *));
    copy_n(try.names.a, fv.names.a, (size_t)fv.names.n * sizeof(char *));
    copy_n(try.types.a, fv.types.a, (size_t)fv.names.n * sizeof(Type *));
    try.names.cap = try.types.cap = fv.names.n + 8;
    if (!liftable(x, NULL, locals, 0, &try) || try.names.n > 4) continue;
    fv = try;
    use[i] = true;
    nuse++;
  }
  if (nuse == L->n && !fv.names.n) { /* a copy must keep a parameter, to stay a function */
    int i = m - 1;
    while (!use[i]) i--;
    use[i] = false;
    nuse--;
  }
  if (!nuse) return;
  TSs ts = {0};
  for (int i = 0; i < m; i++)
    if (L->ptypes[i] && e->args[i]->ty) match_type(L->ptypes[i], e->args[i]->ty, &ts);
  ST->key_fv = &fv;
  SB k = {0};
  sb_printf(&k, "%s@{", fb->name);
  for (int i = 0, c = 0; i < m; i++) {
    if (!use[i]) continue;
    SB a = {0};
    sb_puts(&a, "");
    key(&a, e->args[i], NULL);
    if (a.n > 160) return; /* a growing argument: stop specializing */
    sb_printf(&k, "%s%d=%s", c++ ? ";" : "", i, a.s);
  }
  sb_puts(&k, "}");
  if (ts.n) { /* the types are part of the copy: `map id` differs at [Int] and [Char] */
    TS *saved = ST->tsub;
    int nsaved = ST->ntsub;
    ST->tsub = ts.a;
    ST->ntsub = ts.n;
    sb_puts(&k, "<");
    for (int i = 0; i < L->n; i++) sb_printf(&k, "%s%s", i ? "," : "", L->ptypes[i] ? type_str(apply(L->ptypes[i])) : "$");
    sb_puts(&k, ">");
    ST->tsub = saved;
    ST->ntsub = nsaved;
  }
  const char *name = intern(k.s);
  int nf = fv.names.n;
  if (global_lookup(name) < 0) {
    Bind *b = NEW(Bind);
    *b = (Bind){.name = name, .e = copy_expr(fb->e), .line = fb->line, .col = fb->col, .type = fb->type};
    if (ts.n) {
      TS *saved = ST->tsub;
      int nsaved = ST->ntsub;
      ST->tsub = ts.a;
      ST->ntsub = ts.n;
      subst_types(b->e);
      b->type = apply(b->type);
      ST->tsub = saved;
      ST->ntsub = nsaved;
    }
    Lambda *C = b->e->lam;
    const char **hp = xmalloc((size_t)nf * sizeof *hp + 1);
    Nm *keep = NULL;
    for (int i = 0; i < nf; i++) keep = nm(keep, hp[i] = intern(xsprintf("$h%d", ST->nhparam++)));
    const char **ps = xmalloc((size_t)(C->n + nf) * sizeof *ps);
    Type **ts = xmalloc((size_t)(C->n + nf) * sizeof *ts);
    int n = 0;
    for (int i = 0; i < nf; i++) {
      ps[n] = hp[i];
      ts[n++] = fv.types.a[i];
    }
    ST->keep_locals = keep;
    for (int i = 0; i < C->n; i++) {
      if (i < m && use[i]) {
        Expr *arg = copy_expr(e->args[i]);
        rename_fv(arg, &fv, hp, NULL);
        replace(C->body, C->params[i], arg);
        continue;
      }
      ps[n] = C->params[i];
      ts[n++] = C->ptypes[i];
    }
    ST->keep_locals = NULL;
    C->params = ps;
    C->ptypes = ts;
    C->n = n;
    C->name = name;
    b->gidx = global_add(name, NULL, C);
    CS->globals.a[b->gidx].bind = b;
    ST->nho++;
    VPUSH(CS->specialized, b);
    VPUSH(ST->ho_work, b);
  }
  Expr **rest = xmalloc((size_t)(e->n + nf) * sizeof *rest);
  int n = 0;
  for (int i = 0; i < nf; i++) { /* the call site's locals, as the copy's leading arguments */
    Expr *v = var(fv.names.a[i], f->line, f->col);
    v->ty = fv.types.a[i];
    rest[n++] = v;
  }
  for (int i = 0; i < e->n; i++)
    if (!(i < m && use[i])) rest[n++] = e->args[i];
  Expr *v = var(name, f->line, f->col);
  v->global_ref = true;
  v->ty = f->ty;
  if (n) {
    e->fn = v;
    e->args = rest;
    e->n = n;
  } else {
    Type *t = e->ty;
    *e = *v;
    e->ty = t;
  }
}

static void ho_expr(Expr *e, Nm *s);

static Nm *ho_group(Group *g, Nm *s) {
  for (int i = 0; i < g->n; i++) s = nm(s, g->b[i]->name);
  for (int i = 0; i < g->n; i++) ho_expr(g->b[i]->e, s);
  return s;
}

/* (\p1 .. pn -> body) a1 .. an becomes case a1 of p1 -> .. case an of
   pn -> body: the arguments are still evaluated first, in order, and no
   closure is built. Parameters get fresh names first, so one cannot
   capture a variable of a later argument. */
static void beta(Expr *e) {
  Lambda *L = e->fn->lam;
  int n = L->n;
  Expr *body = L->body;
  const char **fresh = xmalloc((size_t)n * sizeof *fresh);
  for (int i = 0; i < n; i++) {
    fresh[i] = intern(xsprintf("$b%d", ST->nbeta++));
    Expr *v = mk(E_VAR, e->line, e->col);
    v->name = fresh[i];
    v->ty = L->ptypes ? L->ptypes[i] : NULL;
    Nm *saved = ST->keep_locals;
    ST->keep_locals = nm(NULL, fresh[i]); /* the fresh name stays local */
    replace(body, L->params[i], v);
    ST->keep_locals = saved;
  }
  for (int i = n; i-- > 0;) {
    Pat *p = NEW(Pat);
    *p = (Pat){.k = P_VAR, .line = e->line, .col = e->col, .name = fresh[i], .ty = L->ptypes ? L->ptypes[i] : NULL};
    Alt *alt = NEW(Alt);
    alt->np = 1;
    alt->pats = xmalloc(sizeof *alt->pats);
    alt->pats[0] = p;
    alt->rhs.ng = 1;
    alt->rhs.guards = xcalloc(1, sizeof(Expr *));
    alt->rhs.bodies = xmalloc(sizeof(Expr *));
    alt->rhs.bodies[0] = body;
    Expr *c = mk(E_CASE, e->line, e->col);
    c->name = CS->cur_file;
    c->ns = 1;
    c->scruts = xmalloc(sizeof *c->scruts);
    c->scruts[0] = e->args[i];
    c->na = 1;
    c->alts = xmalloc(sizeof *c->alts);
    c->alts[0] = alt;
    c->ty = body->ty;
    body = c;
  }
  if (e->n == n) {
    Type *t = e->ty;
    *e = *body;
    e->ty = t;
  } else { /* more arguments than parameters: apply the result to the rest */
    e->fn = body;
    e->args += n;
    e->n -= n;
  }
}

static void ho_expr(Expr *e, Nm *s) {
  switch (e->k) {
  case E_APP:
    while (e->fn->k == E_APP) { /* (f a) b is f a b */
      Expr *f = e->fn;
      Expr **all = xmalloc((size_t)(f->n + e->n) * sizeof *all);
      copy_n(all, f->args, (size_t)f->n * sizeof *all);
      copy_n(all + f->n, e->args, (size_t)e->n * sizeof *all);
      e->fn = f->fn;
      e->args = all;
      e->n += f->n;
    }
    if (e->fn->k == E_LAM && e->n >= e->fn->lam->n) {
      beta(e);
      ho_expr(e, s);
      return;
    }
    ho_expr(e->fn, s);
    for (int i = 0; i < e->n; i++) ho_expr(e->args[i], s);
    ho_call(e, s);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) s = nm(s, e->lam->params[i]);
    ho_expr(e->lam->body, s);
    return;
  case E_LET: ho_expr(e->body, ho_group(&e->g, s)); return;
  case E_IF:
    ho_expr(e->cond, s);
    ho_expr(e->then_, s);
    ho_expr(e->else_, s);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) ho_expr(e->scruts[i], s);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = s;
      for (int i = 0; i < alt->np; i++) t = pat_nm(alt->pats[i], t);
      for (int i = 0; i < alt->rhs.nwg; i++) t = ho_group(&alt->rhs.wg[i], t);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) ho_expr(alt->rhs.guards[i], t);
        ho_expr(alt->rhs.bodies[i], t);
      }
    }
    return;
  case E_ANN: ho_expr(e->body, s); return;
  default: return;
  }
}

void ho_specialize(Bind **bs, int n) {
  for (int i = 0; i < n; i++) VPUSH(ST->ho_work, bs[i]);
  while (ST->ho_work.n) {
    Bind *b = ST->ho_work.a[--ST->ho_work.n];
    ho_expr(b->e, NULL);
  }
}

/* ---------------------------------------------------------- lambda lifting */

/* A local function that is only ever called -- never passed, returned or
   stored -- needs no closure. It becomes a top-level function taking its
   free local variables as leading parameters, and each call passes them:
   `safe q qs = check qs 1 where check (c:cs) d = .. q ..` calls
   `safe/check q qs 1`. Lifted functions are then ordinary globals: direct
   calls, specialization, TRMC. A group is lifted only if no binder in its
   region rebinds one of its free variables, so the variables passed at a
   call are the ones the function used. */

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
  for (int i = 0; i < g->n; i++) s = binders(g->b[i]->e, nm(s, g->b[i]->name));
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
  case E_LET: return binders(e->body, binders_group(&e->g, s));
  case E_IF: return binders(e->else_, binders(e->then_, binders(e->cond, s)));
  case E_CASE:
    for (int i = 0; i < e->ns; i++) s = binders(e->scruts[i], s);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      for (int i = 0; i < alt->np; i++) s = pat_nm(alt->pats[i], s);
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
  if (fv_index(fv, name) >= 0) return;
  VPUSH(fv->names, name);
  VPUSH(fv->types, t);
}

static Nm *free_group(Group *g, Nm *inner, Nm *locals, Lift *lifts, Fv *fv) {
  for (int i = 0; i < g->n; i++) inner = nm(inner, g->b[i]->name);
  for (int i = 0; i < g->n; i++) free_locals(g->b[i]->e, inner, locals, lifts, fv);
  return inner;
}

static void free_locals(Expr *e, Nm *inner, Nm *locals, Lift *lifts, Fv *fv) {
  switch (e->k) {
  case E_VAR: {
    if (e->global_ref || in(inner, e->name)) return;
    Lift *l = lift_find(lifts, e->name);
    if (l) {
      for (int i = 0; i < l->nfv; i++) add_fv(fv, l->fv[i], l->fvt[i]);
      return;
    }
    if (in(locals, e->name)) add_fv(fv, e->name, e->ty);
    return;
  }
  case E_APP:
    free_locals(e->fn, inner, locals, lifts, fv);
    for (int i = 0; i < e->n; i++) free_locals(e->args[i], inner, locals, lifts, fv);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) inner = nm(inner, e->lam->params[i]);
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
      for (int i = 0; i < alt->np; i++) t = pat_nm(alt->pats[i], t);
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
    for (int k = 0; k < g->n; k++) inner = nm(inner, g->b[k]->name);
    free_locals(g->b[i]->e, inner, locals, lifts, &fv);
  }
  if (fv.names.n > 8) return lifts;
  Nm *bound = binders_group(g, NULL);
  for (int k = 0; k < nrest; k++)
    if (rest[k]) bound = binders(rest[k], bound);
  for (int i = 0; i < fv.names.n; i++)
    if (in(bound, fv.names.a[i])) return lifts; /* a call might see another variable of that name */
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
    for (int k = 0; k < L->n; k++) inner = nm(inner, L->params[k]);
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
    Expr *v = mk(E_VAR, e->line, e->col);
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
    for (int j = 0; j < r->wg[i].n; j++) *locals = nm(*locals, r->wg[i].b[j]->name);
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
    for (int i = 0; i < e->lam->n; i++) locals = nm(locals, e->lam->params[i]);
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
    for (int i = 0; i < e->g.n; i++) locals = nm(locals, e->g.b[i]->name);
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
      for (int i = 0; i < alt->np; i++) t = pat_nm(alt->pats[i], t);
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

/* ------------------------------------------------- reuse of list cells */

/* A call passing a fresh list at a consuming parameter calls a copy,
   `f@reuseK`, which may write its output cells into the input's
   (reuse.md). In the copy, case scrutinees holding the input's spine are
   marked `own`; cgen reuses a cell matched from one for a TRMC output
   cell. Both properties are greatest fixpoints over the chunk's
   functions. Matched cells rebuilt unchanged are replaced by the
   scrutinee first, independently. */

static bool pat_binds(Pat *p, const char *x) { return in(pat_nm(p, NULL), x); }

static Nm *group_nm(Group *g, Nm *s) {
  for (int i = 0; i < g->n; i++) s = nm(s, g->b[i]->name);
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

static Own *own_push(Own *o, const char *name, int mode) {
  Own *n = NEW(Own);
  *n = (Own){name, mode, o};
  return n;
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
    for (Nm *n = pat_nm(p, NULL); n; n = n->next) o = own_push(o, n->name, M_NONE);
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
        for (Nm *n = pat_nm(e->alts[a]->pats[i], NULL); n; n = n->next) t = own_push(t, n->name, M_NONE);
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
  if (g >= 0 && CS->globals.a[g].bind) {
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
        Expr *f = var(own_copy(b, key), e->fn->line, e->fn->col);
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
    for (int i = 0; i < e->lam->n; i++) ps = nm(ps, e->lam->params[i]);
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
      for (int i = 0; i < e->alts[a]->np; i++) names = pat_nm(e->alts[a]->pats[i], names);
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
          if (j != i) others = pat_nm(alt->pats[j], others);
        if (vars && !in(others, s->name)) recon_rhs(&alt->rhs, s, p);
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
}
