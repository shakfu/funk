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

static int dcounter;
__typeof__(specialized) specialized;

typedef struct { Type *from, *to; } TS;
static TS *tsub; /* type variables of the function being specialized */
static int ntsub;

/* t with the variables of tsub replaced. */
static Type *apply(Type *t) {
  t = repr(t);
  if (t->k == TY_VAR) {
    for (int i = 0; i < ntsub; i++)
      if (tsub[i].from == t) return tsub[i].to;
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
  e->ty = dict_type;
  return e;
}

static Expr *app(Expr *f, int n, Expr **args) {
  if (!n) return f;
  Expr *e = mk(E_APP, f->line, f->col);
  e->fn = f;
  e->n = n;
  e->args = args;
  e->ty = dict_type;
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
  ClassDef *c = &classes.a[from];
  for (int i = 0; i < c->nsup; i++) {
    int s = c->sups[i];
    if (!(classes.a[s].closure & ((uint64_t)1 << to))) continue;
    Expr **a = xmalloc(sizeof *a);
    a[0] = e;
    const char *sel = intern(xsprintf("$sup.%s.%s", c->name, classes.a[s].name));
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
      if (d->var == t && (classes.a[d->cls].closure & ((uint64_t)1 << cls)))
        return super_path(d->cls, cls, var(d->name, line, col), line, col);
    cerror(line, col, "ambiguous type: no dictionary for `%s`; add a type signature", classes.a[cls].name);
  }
  InstDef *in = find_inst(cls, t->name);
  if (!in) cerror(line, col, "no instance `%s %s`", classes.a[cls].name, t->name);
  Pred *ps;
  int n = inst_preds(in, &ps);
  Expr **a = xmalloc((size_t)n * sizeof *a + 1);
  for (int i = 0; i < n; i++) {
    int k = 0;
    while (in->vars[k] != ps[i].t) k++;
    a[i] = dict_for(ps[i].cls, t->args[k], env, line, col);
  }
  return app(var(globals.a[in->dict].name, line, col), n, a);
}

static bool builtin_num(Type *t) {
  const char *n = t->name;
  return t->k == TY_CON && (!strcmp(n, "Int") || !strcmp(n, "Float") || !strcmp(n, "Char"));
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
  Bind *ob = globals.a[g].bind;
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
  TS *saved = tsub;
  int nsaved = ntsub;
  VEC(TS) ts = {0};
  for (int i = 0; i < n; i++) {
    VPUSH(ts, ((TS){ps[i].t, tys[i]}));
    for (int k = 0; k < ob->nskolem; k++)
      if (ob->skolem[2 * k] == ps[i].t) VPUSH(ts, ((TS){ob->skolem[2 * k + 1], tys[i]}));
  }
  tsub = ts.a;
  ntsub = ts.n;
  Bind *b = NEW(Bind);
  *b = (Bind){.name = name, .e = copy_expr(ob->orig), .line = ob->line, .col = ob->col};
  b->e->lam->name = name;
  b->type = apply(globals.a[g].type);
  subst_types(b->e);
  b->gidx = global_add(name, b->type, b->e->lam); /* before the body, for recursion */
  elab(b->e, NULL, NULL);
  tsub = saved;
  ntsub = nsaved;
  VPUSH(specialized, b);
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
    n = preds_of(globals.a[g].type, &ps);
  }
  if (!n) return 0;
  if (v->npreds == n) ps = v->preds; /* else a use inside its own group: the binding's own dictionaries */
  Type **tys = xmalloc((size_t)n * sizeof *tys);
  for (int i = 0; i < n; i++) tys[i] = apply(ps[i].t);
  if (g >= 0 && globals.a[g].spec && n == 1 && builtin_num(tys[0])) {
    const char *p = intern(xsprintf("prim%s%s", globals.a[g].spec, tys[0]->name));
    if (global_lookup(p) >= 0) {
      v->name = p;
      return 0;
    }
  }
  InstDef *in;
  if (g >= 0 && globals.a[g].mcls >= 0 && tys[0]->k == TY_CON && (in = find_inst(globals.a[g].mcls, tys[0]->name))) {
    ClassDef *c = &classes.a[in->cls]; /* method at a known instance: $m.C.T.m and its context */
    const char *m = intern(xsprintf("$m.%s.%s.%s", c->name, in->tycon, c->meths[globals.a[g].midx]));
    Type *t0 = tys[0];
    v->name = m;
    g = global_lookup(m);
    n = preds_of(globals.a[g].type, &ps);
    tys = xmalloc((size_t)n * sizeof *tys + 1);
    for (int i = 0; i < n; i++) {
      int k = 0;
      while (in->vars[k] != ps[i].t) k++;
      tys[i] = apply(t0->args[k]);
    }
    if (!n) return 0;
  }
  bool ground = g >= 0 && globals.a[g].bind && globals.a[g].bind->orig;
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
      names[i] = intern(xsprintf("$d%d", dcounter++));
      env = denv(env, ps[i].t, ps[i].cls, names[i]);
      for (int k = 0; k < b->nskolem; k++) /* the body was checked against rigid copies */
        if (b->skolem[2 * k] == ps[i].t) env = denv(env, b->skolem[2 * k + 1], ps[i].cls, names[i]);
    }
    Type **dts = xmalloc((size_t)n * sizeof *dts);
    for (int i = 0; i < n; i++) dts[i] = dict_type;
    if (b->e->k == E_LAM) {
      Lambda *L = b->e->lam;
      const char **all = xmalloc((size_t)(n + L->n) * sizeof *all);
      memcpy(all, names, (size_t)n * sizeof *all);
      memcpy(all + n, L->params, (size_t)L->n * sizeof *all);
      Type **ts = xmalloc((size_t)(n + L->n) * sizeof *ts);
      memcpy(ts, dts, (size_t)n * sizeof *ts);
      memcpy(ts + n, L->ptypes, (size_t)L->n * sizeof *ts);
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
      l->ty = dict_type;
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
    Expr **a = xmalloc(2 * sizeof *a);
    a[0] = dict_for(num_class, t, env, e->line, e->col);
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
        memcpy(all, ds, (size_t)n * sizeof *all);
        memcpy(all + n, e->args, (size_t)e->n * sizeof *all);
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
  ClassDef *c = &classes.a[in->cls];
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
    int k = preds_of(globals.a[global_lookup(m)].type, &mp);
    Expr **a = xmalloc((size_t)k * sizeof *a + 1);
    for (int j = 0; j < k; j++) a[j] = dict_for(mp[j].cls, mp[j].t, env, line, col);
    fields[i] = app(var(m, line, col), k, a);
  }
  for (int i = 0; i < c->nsup; i++) fields[c->nmeth + i] = dict_for(c->sups[i], head, env, line, col);
  Expr *con = mk(E_CON, line, col);
  con->name = condefs.a[c->con].name;
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
  *b = (Bind){.name = globals.a[in->dict].name, .e = body, .line = line, .col = col};
  if (body->k == E_LAM) body->lam->name = b->name;
  return b;
}
