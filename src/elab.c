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
  *b = (Bind){.name = name, .e = copy_expr(ob->orig), .line = ob->line, .col = ob->col, .from = ob->from ? ob->from : ob};
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
    InstDef *in = t->k == TY_CON ? find_inst(num_class, t->name) : NULL;
    int m = in && !in->arity ? global_lookup(intern(xsprintf("$m.Num.%s.fromInt", t->name))) : -1;
    if (m >= 0) { /* a known instance: its fromInt directly, without a dictionary */
      Expr **a = xmalloc(sizeof *a);
      a[0] = lit;
      *e = *app(var(globals.a[m].name, e->line, e->col), 1, a);
      e->ty = t;
      e->fn->idx = m;
      e->fn->known = globals.a[m].lam;
      return;
    }
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

static int nho; /* copies made, bounded so argument growth terminates */

/* Local variables of a call site that an argument uses, in order. */
typedef struct { VEC(const char *) names; VEC(Type *) types; } Fv;

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
static Fv *key_fv;

static void key(SB *b, Expr *e, Nm *params) {
  switch (e->k) {
  case E_VAR: {
    int i = 0;
    for (Nm *s = params; s; s = s->next, i++)
      if (s->name == e->name) {
        sb_printf(b, "#%d", i);
        return;
      }
    if (!e->global_ref && (i = fv_index(key_fv, e->name)) >= 0) {
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

static Nm *keep_locals; /* the copy's leading parameters, during replace */

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
      pin(c, keep_locals);
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
        memmove(alt->pats + i, alt->pats + i + 1, (size_t)(alt->np - i - 1) * sizeof *alt->pats);
        alt->np--;
      }
      memmove(e->scruts + i, e->scruts + i + 1, (size_t)(e->ns - i - 1) * sizeof *e->scruts);
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

static VEC(Bind *) ho_work;

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
static int nhparam;

static void ho_call(Expr *e, Nm *locals) {
  Expr *f = e->fn;
  if (f->k != E_VAR || (!f->global_ref && in(locals, f->name))) return;
  int g = global_lookup(f->name);
  Bind *fb = g >= 0 ? globals.a[g].bind : NULL;
  if (!fb || fb->e->k != E_LAM || !fb->e->lam->ptypes || nho >= 4000) return;
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
    memcpy(try.names.a, fv.names.a, (size_t)fv.names.n * sizeof(char *));
    memcpy(try.types.a, fv.types.a, (size_t)fv.names.n * sizeof(Type *));
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
  key_fv = &fv;
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
    TS *saved = tsub;
    int nsaved = ntsub;
    tsub = ts.a;
    ntsub = ts.n;
    sb_puts(&k, "<");
    for (int i = 0; i < L->n; i++) sb_printf(&k, "%s%s", i ? "," : "", L->ptypes[i] ? type_str(apply(L->ptypes[i])) : "$");
    sb_puts(&k, ">");
    tsub = saved;
    ntsub = nsaved;
  }
  const char *name = intern(k.s);
  int nf = fv.names.n;
  if (global_lookup(name) < 0) {
    Bind *b = NEW(Bind);
    *b = (Bind){.name = name, .e = copy_expr(fb->e), .line = fb->line, .col = fb->col, .type = fb->type};
    if (ts.n) {
      TS *saved = tsub;
      int nsaved = ntsub;
      tsub = ts.a;
      ntsub = ts.n;
      subst_types(b->e);
      b->type = apply(b->type);
      tsub = saved;
      ntsub = nsaved;
    }
    Lambda *C = b->e->lam;
    const char **hp = xmalloc((size_t)nf * sizeof *hp + 1);
    Nm *keep = NULL;
    for (int i = 0; i < nf; i++) keep = nm(keep, hp[i] = intern(xsprintf("$h%d", nhparam++)));
    const char **ps = xmalloc((size_t)(C->n + nf) * sizeof *ps);
    Type **ts = xmalloc((size_t)(C->n + nf) * sizeof *ts);
    int n = 0;
    for (int i = 0; i < nf; i++) {
      ps[n] = hp[i];
      ts[n++] = fv.types.a[i];
    }
    keep_locals = keep;
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
    keep_locals = NULL;
    C->params = ps;
    C->ptypes = ts;
    C->n = n;
    C->name = name;
    b->gidx = global_add(name, NULL, C);
    globals.a[b->gidx].bind = b;
    nho++;
    VPUSH(specialized, b);
    VPUSH(ho_work, b);
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
static int nbeta;

static void beta(Expr *e) {
  Lambda *L = e->fn->lam;
  int n = L->n;
  Expr *body = L->body;
  const char **fresh = xmalloc((size_t)n * sizeof *fresh);
  for (int i = 0; i < n; i++) {
    fresh[i] = intern(xsprintf("$b%d", nbeta++));
    Expr *v = mk(E_VAR, e->line, e->col);
    v->name = fresh[i];
    v->ty = L->ptypes ? L->ptypes[i] : NULL;
    Nm *saved = keep_locals;
    keep_locals = nm(NULL, fresh[i]); /* the fresh name stays local */
    replace(body, L->params[i], v);
    keep_locals = saved;
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
    c->name = cur_file;
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
      memcpy(all, f->args, (size_t)f->n * sizeof *all);
      memcpy(all + f->n, e->args, (size_t)e->n * sizeof *all);
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
  for (int i = 0; i < n; i++) VPUSH(ho_work, bs[i]);
  while (ho_work.n) {
    Bind *b = ho_work.a[--ho_work.n];
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

static int nlift;

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

static VEC(Bind *) lifted;

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
    *l = (Lift){g->b[i]->name, intern(xsprintf("%s/%s%d", owner, g->b[i]->name, nlift++)), fv.names.n, fv.names.a,
                fv.types.a, lifts};
    lifts = l;
  }
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    Lambda *L = b->e->lam;
    Lift *l = lift_find(lifts, b->name);
    const char **ps = xmalloc((size_t)(l->nfv + L->n) * sizeof *ps);
    Type **ts = xmalloc((size_t)(l->nfv + L->n) * sizeof *ts);
    memcpy(ps, l->fv, (size_t)l->nfv * sizeof *ps);
    memcpy(ts, l->fvt, (size_t)l->nfv * sizeof *ts);
    memcpy(ps + l->nfv, L->params, (size_t)L->n * sizeof *ps);
    memcpy(ts + l->nfv, L->ptypes, (size_t)L->n * sizeof *ts);
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
    globals.a[nb->gidx].bind = nb;
    VPUSH(lifted, nb);
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
  memcpy(all + l->nfv, e->args, (size_t)e->n * sizeof *all);
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
      memmove(r->wg + i, r->wg + i + 1, (size_t)(r->nwg - i - 1) * sizeof *r->wg);
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
  for (int i = 0; i < lifted.n; i++) VPUSH(specialized, lifted.a[i]);
  lifted.n = 0;
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

static bool is_list(Type *t) {
  t = t ? repr(t) : NULL;
  return t && t->k == TY_CON && !strcmp(t->name, "[]");
}

/* The top-level function f calls, unless a local shadows it. */
static Bind *callee(Expr *f, Nm *loc) {
  if (f->k != E_VAR || (!f->global_ref && in(loc, f->name))) return NULL;
  int g = global_lookup(f->name);
  Bind *b = g >= 0 ? globals.a[g].bind : NULL;
  return b && b->e->k == E_LAM && b->e->lam->ptypes ? b : NULL;
}

/* Does e evaluate to cells nothing else references? */
static bool fresh(Expr *e, Nm *loc) {
  switch (e->k) {
  case E_CON: return e->idx == FK_C_NIL;
  case E_ANN: return fresh(e->body, loc);
  case E_LET: return fresh(e->body, group_nm(&e->g, loc));
  case E_IF: return fresh(e->then_, loc) && fresh(e->else_, loc);
  case E_CASE:
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = loc;
      for (int i = 0; i < alt->np; i++) t = pat_nm(alt->pats[i], t);
      for (int i = 0; i < alt->rhs.nwg; i++) t = group_nm(&alt->rhs.wg[i], t);
      for (int i = 0; i < alt->rhs.ng; i++)
        if (!fresh(alt->rhs.bodies[i], t)) return false;
    }
    return true;
  case E_APP: {
    if (e->fn->k == E_CON) return e->fn->idx == FK_C_CONS && e->n == 2 && fresh(e->args[1], loc);
    Bind *b = callee(e->fn, loc);
    if (b) return b->fresh && e->n == b->e->lam->n;
    Expr *f = e->fn; /* error never returns */
    return f->k == E_VAR && (f->global_ref || !in(loc, f->name)) && !strcmp(f->name, "error");
  }
  default: return false;
  }
}

/* The variable continuing a matched list's spine: the pattern itself if a
   variable, else the innermost tail of its cons cells. */
static const char *spine_rest(Pat *p) {
  while (p->k == P_CON && p->con == FK_C_CONS) p = p->args[1];
  return p->k == P_VAR ? p->name : NULL;
}

static bool consumes(Expr *e, const char *v, Nm *loc);

static bool consumes_rhs(Rhs *r, const char *v, Nm *loc) {
  if (!occ_rhs(r, v)) return true;
  for (int i = 0; i < r->nwg; i++) {
    if (binds_in(&r->wg[i], v) || occ_group(&r->wg[i], v)) return false;
    loc = group_nm(&r->wg[i], loc);
  }
  for (int i = 0; i < r->ng; i++)
    if ((r->guards[i] && occ(r->guards[i], v)) || !consumes(r->bodies[i], v, loc)) return false;
  return true;
}

/* Is the list v taken apart at most once on each path through e, with no
   reference kept to a cell taken apart? */
static bool consumes(Expr *e, const char *v, Nm *loc) {
  if (!occ(e, v)) return true;
  switch (e->k) {
  case E_ANN: return consumes(e->body, v, loc);
  case E_LET:
    if (binds_in(&e->g, v) || occ_group(&e->g, v)) return false;
    return consumes(e->body, v, group_nm(&e->g, loc));
  case E_IF:
    if (occ(e->cond, v)) return !occ(e->then_, v) && !occ(e->else_, v) && consumes(e->cond, v, loc);
    return consumes(e->then_, v, loc) && consumes(e->else_, v, loc);
  case E_APP: {
    if (occ(e->fn, v)) return false;
    int k = -1;
    for (int i = 0; i < e->n; i++)
      if (occ(e->args[i], v)) {
        if (k >= 0) return false;
        k = i;
      }
    Expr *a = e->args[k];
    if (a->k != E_VAR) return consumes(a, v, loc);
    Bind *b = callee(e->fn, loc);
    return b && e->n >= b->e->lam->n && k < 32 && (b->consumes >> k & 1);
  }
  case E_CASE: {
    int k = -1;
    for (int i = 0; i < e->ns; i++)
      if (occ(e->scruts[i], v)) {
        if (k >= 0) return false;
        k = i;
      }
    if (k >= 0 && e->scruts[k]->k != E_VAR) { /* taken apart inside the scrutinee */
      for (int a = 0; a < e->na; a++)
        if (occ_rhs(&e->alts[a]->rhs, v)) return false;
      return consumes(e->scruts[k], v, loc);
    }
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = loc;
      bool shadow = false;
      for (int i = 0; i < alt->np; i++) {
        t = pat_nm(alt->pats[i], t);
        shadow |= pat_binds(alt->pats[i], v);
      }
      const char *w = v;
      if (k >= 0) { /* the matched cell is dead; its tail continues the spine */
        w = spine_rest(alt->pats[k]);
        if (!shadow && occ_rhs(&alt->rhs, v)) return false;
      } else if (shadow) {
        if (occ_rhs(&alt->rhs, v)) return false;
        continue;
      }
      if (w && !consumes_rhs(&alt->rhs, w, t)) return false;
    }
    return true;
  }
  default: return false; /* returned, stored or captured */
  }
}

/* A tail result `x : self ...`, where cgen's TRMC allocates a cell. */
static bool trmc_site(Expr *e, const char *self) {
  switch (e->k) {
  case E_APP:
    return e->fn->k == E_CON && e->fn->idx == FK_C_CONS && e->n == 2 && e->args[1]->k == E_APP &&
           e->args[1]->fn->k == E_VAR && e->args[1]->fn->name == self;
  case E_LET: return trmc_site(e->body, self);
  case E_IF: return trmc_site(e->then_, self) || trmc_site(e->else_, self);
  case E_ANN: return trmc_site(e->body, self);
  case E_CASE:
    for (int a = 0; a < e->na; a++)
      for (int i = 0; i < e->alts[a]->rhs.ng; i++)
        if (trmc_site(e->alts[a]->rhs.bodies[i], self)) return true;
    return false;
  default: return false;
  }
}

static VEC(Bind *) reuse_work;

static const char *reuse_copy(Bind *fb, int k) {
  const char *name = intern(xsprintf("%s@reuse%d", fb->name, k));
  if (global_lookup(name) < 0) {
    Bind *b = NEW(Bind);
    *b = (Bind){.name = name, .e = copy_expr(fb->e), .line = fb->line, .col = fb->col, .type = fb->type,
                .fresh = fb->fresh, .consumes = fb->consumes, .gain = fb->gain, .reuse = k + 1};
    b->e->lam->name = name;
    b->gidx = global_add(name, NULL, b->e->lam);
    globals.a[b->gidx].bind = b;
    VPUSH(specialized, b);
    VPUSH(reuse_work, b);
  }
  return name;
}

/* Redirect calls passing a fresh list, or the spine v of a reuse copy, at
   a parameter whose cells can be reused; mark v's scrutinees. */
static void rw(Expr *e, const char *v, Nm *loc);

static void rw_rhs(Rhs *r, const char *v, Nm *loc) {
  for (int i = 0; i < r->nwg; i++) {
    if (binds_in(&r->wg[i], v)) v = NULL;
    loc = group_nm(&r->wg[i], loc);
    for (int j = 0; j < r->wg[i].n; j++) rw(r->wg[i].b[j]->e, NULL, loc);
  }
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) rw(r->guards[i], NULL, loc);
    rw(r->bodies[i], v, loc);
  }
}

static void rw(Expr *e, const char *v, Nm *loc) {
  switch (e->k) {
  case E_APP: {
    Bind *b = callee(e->fn, loc);
    for (int i = 0; b && !b->reuse && i < e->n && i < b->e->lam->n && i < 32; i++) {
      Expr *a = e->args[i];
      if (!(b->gain >> i & 1) || e->n < b->e->lam->n) continue;
      if ((v && a->k == E_VAR && a->name == v && !a->global_ref) || fresh(a, loc)) {
        Expr *f = var(reuse_copy(b, i), e->fn->line, e->fn->col);
        f->global_ref = true;
        f->ty = e->fn->ty;
        e->fn = f;
        break;
      }
    }
    rw(e->fn, NULL, loc);
    for (int i = 0; i < e->n; i++) rw(e->args[i], v, loc);
    return;
  }
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) loc = nm(loc, e->lam->params[i]);
    rw(e->lam->body, NULL, loc);
    return;
  case E_LET:
    loc = group_nm(&e->g, loc);
    for (int i = 0; i < e->g.n; i++) rw(e->g.b[i]->e, NULL, loc);
    rw(e->body, binds_in(&e->g, v) ? NULL : v, loc);
    return;
  case E_IF:
    rw(e->cond, v, loc);
    rw(e->then_, v, loc);
    rw(e->else_, v, loc);
    return;
  case E_CASE: {
    int k = -1;
    for (int i = 0; i < e->ns; i++) {
      Expr *s = e->scruts[i];
      if (v && s->k == E_VAR && s->name == v && !s->global_ref) {
        s->own = true;
        k = i;
      }
      rw(s, v, loc);
    }
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Nm *t = loc;
      const char *w = v;
      for (int i = 0; i < alt->np; i++) {
        t = pat_nm(alt->pats[i], t);
        if (pat_binds(alt->pats[i], v)) w = NULL;
      }
      if (k >= 0) w = spine_rest(alt->pats[k]);
      rw_rhs(&alt->rhs, w, t);
    }
    return;
  }
  case E_ANN: rw(e->body, v, loc); return;
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
  for (int i = 0; i < all.n; i++) {
    Bind *b = all.a[i];
    if (b->e->k == E_LAM && !globals.a[b->gidx].bind) globals.a[b->gidx].bind = b; /* type-class clones */
    recon(b->e);
    if (b->e->k != E_LAM || !b->e->lam->ptypes) continue;
    Lambda *L = b->e->lam;
    b->fresh = true;
    b->consumes = 0;
    for (int j = 0; j < L->n && j < 32; j++)
      if (is_list(L->ptypes[j])) b->consumes |= 1u << j;
    VPUSH(fns, b);
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (int i = 0; i < fns.n; i++) {
      Bind *b = fns.a[i];
      Lambda *L = b->e->lam;
      Nm *loc = NULL;
      for (int j = 0; j < L->n; j++) loc = nm(loc, L->params[j]);
      if (b->fresh && !fresh(L->body, loc)) {
        b->fresh = false;
        changed = true;
      }
      for (int j = 0; j < L->n && j < 32; j++)
        if ((b->consumes >> j & 1) && !consumes(L->body, L->params[j], loc)) {
          b->consumes &= ~(1u << j);
          changed = true;
        }
    }
  }
  for (int i = 0; i < fns.n; i++) fns.a[i]->gain = trmc_site(fns.a[i]->e->lam->body, fns.a[i]->name) ? fns.a[i]->consumes : 0;
  for (int i = 0; i < all.n; i++) rw(all.a[i]->e, NULL, NULL);
  while (reuse_work.n) {
    Bind *b = reuse_work.a[--reuse_work.n];
    Lambda *L = b->e->lam;
    Nm *loc = NULL;
    for (int j = 0; j < L->n; j++) loc = nm(loc, L->params[j]);
    rw(L->body, L->params[b->reuse - 1], loc);
  }
}
