/* check.c -- dependency analysis and Hindley-Milner type inference.

   Inference is algorithm J with mutable unification variables and
   levels for generalization (Remy; as in OCaml). Generalized variables are
   marked in place with level GENERIC; instantiation copies them.

   Numbers: a type variable may carry a `num` flag, written `Num a =>`.
   Such a variable can only unify with Int, Float, or another variable,
   which inherits the flag. Integer literals get a fresh num variable, so
   `x + 1` works for both Int and Float without type classes.

   Monomorphism restriction: a binding that is not syntactically a lambda
   does not generalize its num variables. They stay open until a later use
   fixes them, else default to Int at the end of the chunk. Without this,
   `x = 1` would be polymorphic and its runtime value would be an Int even
   where it is used as a Float. */
#include <limits.h>

#include "funk.h"

#define GENERIC INT_MAX

__typeof__(globals) globals;
__typeof__(condefs) condefs;
__typeof__(tydefs) tydefs;
__typeof__(all_lambdas) all_lambdas;

static Map gmap, cmap, tmap;
static int level;
static int tvid;
static VEC(Type *) numvars;
static const char *s_int, *s_float, *s_char, *s_bool, *s_list, *s_arrow, *s_unit, *s_string;

static const struct { const char *cname, *type; } prim_info[] = {
#define P(c, name, ar, ty) {#c, ty},
#include "prims.def"
#undef P
};

const char *prim_cname(int i) { return prim_info[i].cname; }

int global_lookup(const char *name) { return map_get(&gmap, name); }

void global_alias(const char *name, int gidx) { map_put(&gmap, name, gidx); }

/* ----------------------------------------------------------------- types */

static Type *tcon(const char *name, int n, Type **args) {
  Type *t = NEW(Type);
  t->k = TY_CON;
  t->name = name;
  t->n = n;
  if (n) {
    t->args = xmalloc((size_t)n * sizeof *args);
    memcpy(t->args, args, (size_t)n * sizeof *args);
  }
  return t;
}

static Type *tcon0(const char *name) { return tcon(name, 0, NULL); }
static Type *tarrow(Type *a, Type *b) { return tcon(s_arrow, 2, (Type *[]){a, b}); }
static Type *tlist(Type *a) { return tcon(s_list, 1, &a); }

static Type *newvar(void) {
  Type *t = NEW(Type);
  t->k = TY_VAR;
  t->level = level;
  t->id = tvid++;
  return t;
}

static Type *numvar(void) {
  Type *t = newvar();
  t->num = true;
  VPUSH(numvars, t);
  return t;
}

static Type *gvar(bool num) {
  Type *t = newvar();
  t->level = GENERIC;
  t->num = num;
  return t;
}

Type *repr(Type *t) {
  while (t->k == TY_VAR && t->link) {
    if (t->link->k == TY_VAR && t->link->link) t->link = t->link->link;
    t = t->link;
  }
  return t;
}

static bool is_tuple_name(const char *s) { return s[0] == '(' && s[1] == ','; }

/* -------------------------------------------------------------- printing */

typedef VEC(Type *) Names;

static void pr(SB *b, Type *t, int prec, Names *nm) {
  t = repr(t);
  if (t->k == TY_VAR) {
    int i = 0;
    while (i < nm->n && nm->a[i] != t) i++;
    if (i == nm->n) VPUSH(*nm, t);
    if (i < 26) sb_printf(b, "%c", 'a' + i);
    else sb_printf(b, "t%d", i);
    return;
  }
  if (t->name == s_arrow) {
    if (prec > 0) sb_puts(b, "(");
    pr(b, t->args[0], 1, nm);
    sb_puts(b, " -> ");
    pr(b, t->args[1], 0, nm);
    if (prec > 0) sb_puts(b, ")");
  } else if (t->name == s_list) {
    sb_puts(b, "[");
    pr(b, t->args[0], 0, nm);
    sb_puts(b, "]");
  } else if (is_tuple_name(t->name)) {
    sb_puts(b, "(");
    for (int i = 0; i < t->n; i++) {
      if (i) sb_puts(b, ", ");
      pr(b, t->args[i], 0, nm);
    }
    sb_puts(b, ")");
  } else {
    if (t->n && prec > 1) sb_puts(b, "(");
    sb_puts(b, t->name);
    for (int i = 0; i < t->n; i++) {
      sb_puts(b, " ");
      pr(b, t->args[i], 2, nm);
    }
    if (t->n && prec > 1) sb_puts(b, ")");
  }
}

static char *tstr(Type *t, Names *nm) {
  SB b = {0};
  sb_puts(&b, "");
  pr(&b, t, 0, nm);
  return b.s;
}

const char *type_str(Type *t) {
  Names nm = {0};
  char *body = tstr(t, &nm);
  SB ctx = {0};
  sb_puts(&ctx, "");
  int nnum = 0;
  for (int i = 0; i < nm.n; i++) nnum += nm.a[i]->num;
  if (nnum) {
    if (nnum > 1) sb_puts(&ctx, "(");
    for (int i = 0, k = 0; i < nm.n; i++) {
      if (!nm.a[i]->num) continue;
      if (k++) sb_puts(&ctx, ", ");
      sb_puts(&ctx, "Num ");
      if (i < 26) sb_printf(&ctx, "%c", 'a' + i);
      else sb_printf(&ctx, "t%d", i);
    }
    sb_puts(&ctx, nnum > 1 ? ") => " : " => ");
  }
  sb_puts(&ctx, body);
  return ctx.s;
}

/* ----------------------------------------------------------- unification */

static int uline, ucol;
static Type *outer_exp, *outer_act;

static _Noreturn void mismatch(Type *a, Type *b) {
  Names nm = {0};
  char *oa = tstr(outer_exp, &nm), *ob = tstr(outer_act, &nm);
  char *sa = tstr(a, &nm), *sb = tstr(b, &nm);
  if (strcmp(sa, oa) == 0 && strcmp(sb, ob) == 0)
    cerror(uline, ucol, "type mismatch: expected `%s`, found `%s`", sa, sb);
  cerror(uline, ucol, "type mismatch: expected `%s`, found `%s`\n  (cannot match `%s` with `%s`)", oa, ob, sa, sb);
}

static _Noreturn void escape_error(void) {
  cerror(uline, ucol, "type signature is too general: a type variable in it is fixed by the enclosing scope");
}

static void occurs_adjust(Type *v, Type *t) {
  t = repr(t);
  if (t == v) {
    Names nm = {0};
    char *sv = tstr(v, &nm), *so = tstr(outer_act, &nm);
    cerror(uline, ucol, "infinite type: `%s` occurs in `%s`", sv, so);
  }
  if (t->k == TY_VAR) {
    if (t->skolem) {
      if (t->level > v->level) escape_error();
    } else if (t->level > v->level) {
      t->level = v->level;
    }
    return;
  }
  for (int i = 0; i < t->n; i++) occurs_adjust(v, t->args[i]);
}

static void bind_var(Type *v, Type *t) {
  if (t->k == TY_VAR) {
    if (t->skolem) {
      if (v->num && !t->num) {
        Names nm = {0};
        cerror(uline, ucol, "no Num instance for rigid type variable `%s`; add `Num %s =>` to the signature",
               tstr(t, &nm), tstr(t, &nm));
      }
      if (t->level > v->level) escape_error();
    } else {
      if (v->num && !t->num) {
        t->num = true;
        VPUSH(numvars, t);
      }
      if (v->level < t->level) t->level = v->level;
    }
    v->link = t;
    return;
  }
  if (v->num && t->name != s_int && t->name != s_float) {
    Names nm = {0};
    cerror(uline, ucol, "no Num instance for `%s` (numbers are Int or Float)", tstr(t, &nm));
  }
  occurs_adjust(v, t);
  v->link = t;
}

static void unify_rec(Type *a, Type *b) {
  a = repr(a);
  b = repr(b);
  if (a == b) return;
  if (a->k == TY_VAR && !a->skolem) { bind_var(a, b); return; }
  if (b->k == TY_VAR && !b->skolem) { bind_var(b, a); return; }
  if (a->k == TY_CON && b->k == TY_CON && a->name == b->name && a->n == b->n) {
    for (int i = 0; i < a->n; i++) unify_rec(a->args[i], b->args[i]);
    return;
  }
  mismatch(a, b);
}

static void unify(Type *expected, Type *actual, int line, int col) {
  uline = line;
  ucol = col;
  outer_exp = expected;
  outer_act = actual;
  unify_rec(expected, actual);
}

/* ------------------------------------------------- instantiate/generalize */

typedef VEC(Type *) Subst; /* pairs: generic, replacement */

static Type *inst_rec(Type *t, Subst *s, bool skolem) {
  t = repr(t);
  if (t->k == TY_VAR) {
    if (t->level != GENERIC) return t;
    for (int i = 0; i < s->n; i += 2)
      if (s->a[i] == t) return s->a[i + 1];
    Type *v = newvar();
    v->num = t->num;
    v->skolem = skolem;
    if (v->num && !skolem) VPUSH(numvars, v);
    VPUSH(*s, t);
    VPUSH(*s, v);
    return v;
  }
  if (!t->n) return t;
  Type **args = xmalloc((size_t)t->n * sizeof *args);
  bool same = true;
  for (int i = 0; i < t->n; i++) {
    args[i] = inst_rec(t->args[i], s, skolem);
    same &= args[i] == t->args[i];
  }
  if (same) { free(args); return t; }
  Type *r = tcon(t->name, t->n, args);
  free(args);
  return r;
}

static Type *inst(Type *t, bool skolem) {
  Subst s = {0};
  Type *r = inst_rec(t, &s, skolem);
  free(s.a);
  return r;
}

static void gen(Type *t, bool mr) {
  t = repr(t);
  if (t->k == TY_VAR) {
    if (!t->skolem && t->level != GENERIC && t->level > level) t->level = mr && t->num ? level : GENERIC;
    return;
  }
  for (int i = 0; i < t->n; i++) gen(t->args[i], mr);
}

/* --------------------------------------------------------- type syntax */

typedef VEC(const char *) Strs;
typedef struct { Strs names; VEC(Type *) types; } TVars;

static Type *te_type(TypeExpr *te, TVars *tv, Sig *sig) {
  switch (te->k) {
  case TE_VAR:
    for (int i = 0; i < tv->names.n; i++)
      if (tv->names.a[i] == te->name) return tv->types.a[i];
    if (!sig) cerror(te->line, te->col, "type variable `%s` is not a parameter of the data type", te->name);
    {
      bool num = false;
      for (int i = 0; i < sig->nctx; i++) num |= sig->ctx[i] == te->name;
      Type *v = gvar(num);
      VPUSH(tv->names, te->name);
      VPUSH(tv->types, v);
      return v;
    }
  case TE_FUN: return tarrow(te_type(te->args[0], tv, sig), te_type(te->args[1], tv, sig));
  case TE_LIST: return tlist(te_type(te->args[0], tv, sig));
  case TE_TUPLE: {
    if (te->n == 0) return tcon0(s_unit);
    if (te->n > FK_MAXTUPLE) cerror(te->line, te->col, "tuples have at most %d elements", FK_MAXTUPLE);
    char buf[16] = "(";
    for (int i = 1; i < te->n; i++) strcat(buf, ",");
    strcat(buf, ")");
    Type **args = xmalloc((size_t)te->n * sizeof *args);
    for (int i = 0; i < te->n; i++) args[i] = te_type(te->args[i], tv, sig);
    return tcon(intern(buf), te->n, args);
  }
  case TE_CON: {
    int ti = map_get(&tmap, te->name);
    if (ti < 0 && te->name == s_string && te->n == 0) return tlist(tcon0(s_char));
    if (ti < 0) cerror(te->line, te->col, "unknown type `%s`", te->name);
    if (tydefs.a[ti].arity != te->n)
      cerror(te->line, te->col, "type `%s` expects %d argument%s, got %d", te->name, tydefs.a[ti].arity,
             tydefs.a[ti].arity == 1 ? "" : "s", te->n);
    Type **args = te->n ? xmalloc((size_t)te->n * sizeof *args) : NULL;
    for (int i = 0; i < te->n; i++) args[i] = te_type(te->args[i], tv, sig);
    return tcon(tydefs.a[ti].name, te->n, args);
  }
  }
  abort();
}

static Type *sig_scheme(Sig *s) {
  TVars tv = {0};
  return te_type(s->t, &tv, s);
}

/* ----------------------------------------------------------------- data */

static void register_datas(Module *m) {
  for (int i = 0; i < m->nd; i++) {
    DataDecl *d = m->datas[i];
    for (int j = 0; j < i; j++)
      if (m->datas[j]->name == d->name) cerror(d->line, d->col, "duplicate data type `%s`", d->name);
    int ti = map_get(&tmap, d->name);
    if (ti >= 0 && ti < 8) cerror(d->line, d->col, "cannot redefine builtin type `%s`", d->name);
    VPUSH(tydefs, ((TyDef){d->name, d->np, d->nc}));
    map_put(&tmap, d->name, tydefs.n - 1);
  }
  for (int i = 0; i < m->nd; i++) {
    DataDecl *d = m->datas[i];
    TVars tv = {0};
    for (int j = 0; j < d->np; j++) {
      for (int k = 0; k < j; k++)
        if (d->params[k] == d->params[j]) cerror(d->line, d->col, "duplicate type parameter `%s`", d->params[j]);
      VPUSH(tv.names, d->params[j]);
      VPUSH(tv.types, gvar(false));
    }
    Type *res = tcon(d->name, d->np, tv.types.a);
    for (int c = 0; c < d->nc; c++) {
      ConDecl *cd = &d->cons[c];
      int prev = map_get(&cmap, cd->name);
      if (prev >= (int)FK_NBUILTIN_CONS && condefs.a[prev].tydef >= tydefs.n - m->nd)
        cerror(cd->line, cd->col, "duplicate constructor `%s`", cd->name);
      if (prev >= 0 && prev < (int)FK_NBUILTIN_CONS)
        cerror(cd->line, cd->col, "cannot redefine builtin constructor `%s`", cd->name);
      Type *t = res;
      for (int f = cd->nfields; f-- > 0;) t = tarrow(te_type(cd->fields[f], &tv, NULL), t);
      uint32_t id = fk_register_con(cd->name, cd->nfields);
      if ((int)id != condefs.n) abort();
      VPUSH(condefs, ((ConDef){cd->name, cd->nfields, t, map_get(&tmap, d->name)}));
      map_put(&cmap, cd->name, (int)id);
    }
  }
}

/* ---------------------------------------------------------- dependencies */

typedef struct Sh { const char *name; struct Sh *next; } Sh;
typedef struct { Bind **b; int n, cur; bool *adj; } Deps;

static Sh *shpush(Sh *s, const char *name) {
  Sh *n = NEW(Sh);
  n->name = name;
  n->next = s;
  return n;
}

static bool shadowed(Sh *s, const char *name) {
  for (; s; s = s->next)
    if (s->name == name) return true;
  return false;
}

static void deps_expr(Expr *e, Sh *sh, Deps *d);

static Sh *deps_pat(Pat *p, Sh *sh) {
  if (p->k == P_VAR) return shpush(sh, p->name);
  for (int i = 0; i < p->n; i++) sh = deps_pat(p->args[i], sh);
  return sh;
}

static Sh *deps_group(Group *g, Sh *sh, Deps *d) {
  for (int i = 0; i < g->n; i++) sh = shpush(sh, g->b[i]->name);
  for (int i = 0; i < g->n; i++) deps_expr(g->b[i]->e, sh, d);
  return sh;
}

static void deps_expr(Expr *e, Sh *sh, Deps *d) {
  switch (e->k) {
  case E_VAR:
    if (shadowed(sh, e->name)) return;
    for (int i = 0; i < d->n; i++)
      if (d->b[i]->name == e->name) d->adj[d->cur * d->n + i] = true;
    return;
  case E_APP:
    deps_expr(e->fn, sh, d);
    for (int i = 0; i < e->n; i++) deps_expr(e->args[i], sh, d);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) sh = shpush(sh, e->lam->params[i]);
    deps_expr(e->lam->body, sh, d);
    return;
  case E_LET: deps_expr(e->body, deps_group(&e->g, sh, d), d); return;
  case E_IF:
    deps_expr(e->cond, sh, d);
    deps_expr(e->then_, sh, d);
    deps_expr(e->else_, sh, d);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) deps_expr(e->scruts[i], sh, d);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Sh *s = sh;
      for (int i = 0; i < alt->np; i++) s = deps_pat(alt->pats[i], s);
      for (int i = 0; i < alt->rhs.nwg; i++) s = deps_group(&alt->rhs.wg[i], s, d);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) deps_expr(alt->rhs.guards[i], s, d);
        deps_expr(alt->rhs.bodies[i], s, d);
      }
    }
    return;
  case E_ANN: deps_expr(e->body, sh, d); return;
  default: return;
  }
}

typedef struct {
  Deps *d;
  int counter, sp;
  int *index, *low, *stack;
  bool *on;
  VEC(Group) out;
} Tarjan;

static void strong(Tarjan *T, int v) {
  int n = T->d->n;
  T->index[v] = T->low[v] = T->counter++;
  T->stack[T->sp++] = v;
  T->on[v] = true;
  for (int w = 0; w < n; w++) {
    if (!T->d->adj[v * n + w]) continue;
    if (T->index[w] < 0) {
      strong(T, w);
      if (T->low[w] < T->low[v]) T->low[v] = T->low[w];
    } else if (T->on[w] && T->index[w] < T->low[v]) {
      T->low[v] = T->index[w];
    }
  }
  if (T->low[v] != T->index[v]) return;
  Group g = {0};
  VEC(Bind *) bs = {0};
  int w;
  do {
    w = T->stack[--T->sp];
    T->on[w] = false;
    VPUSH(bs, T->d->b[w]);
  } while (w != v);
  /* restore source order within the group */
  for (int i = 0; i < bs.n / 2; i++) {
    Bind *t = bs.a[i];
    bs.a[i] = bs.a[bs.n - 1 - i];
    bs.a[bs.n - 1 - i] = t;
  }
  g.b = bs.a;
  g.n = bs.n;
  g.rec = bs.n > 1 || T->d->adj[v * n + v];
  if (g.rec)
    for (int i = 0; i < g.n; i++)
      if (g.b[i]->e->k != E_LAM)
        cerror(g.b[i]->line, g.b[i]->col,
               "`%s` is defined recursively but is not a function (evaluation is strict)", g.b[i]->name);
  VPUSH(T->out, g);
}

/* Split bindings into strongly connected components in dependency order. */
static Group *scc(Bind **b, int n, int *ng) {
  Deps d = {b, n, 0, xcalloc((size_t)n * (size_t)n + 1, 1)};
  for (int i = 0; i < n; i++) {
    d.cur = i;
    deps_expr(b[i]->e, NULL, &d);
  }
  Tarjan T = {&d, 0, 0, xmalloc((size_t)n * sizeof(int) + 1), xmalloc((size_t)n * sizeof(int) + 1),
              xmalloc((size_t)n * sizeof(int) + 1), xcalloc((size_t)n + 1, 1), {0}};
  for (int i = 0; i < n; i++) T.index[i] = -1;
  for (int i = 0; i < n; i++)
    if (T.index[i] < 0) strong(&T, i);
  free(d.adj);
  free(T.index);
  free(T.low);
  free(T.stack);
  free(T.on);
  *ng = T.out.n;
  return T.out.a;
}

static void split_expr(Expr *e);

static void split_group_exprs(Group *g) {
  for (int i = 0; i < g->n; i++) {
    if (g->b[i]->e->k == E_LAM && !g->b[i]->e->lam->name) g->b[i]->e->lam->name = g->b[i]->name;
    split_expr(g->b[i]->e);
  }
}

static void split_rhs(Rhs *r) {
  if (r->nwg == 1 && r->wg[0].n) {
    split_group_exprs(&r->wg[0]);
    r->wg = scc(r->wg[0].b, r->wg[0].n, &r->nwg);
  }
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) split_expr(r->guards[i]);
    split_expr(r->bodies[i]);
  }
}

static void split_expr(Expr *e) {
  switch (e->k) {
  case E_APP:
    split_expr(e->fn);
    for (int i = 0; i < e->n; i++) split_expr(e->args[i]);
    return;
  case E_LAM: split_expr(e->lam->body); return;
  case E_LET: {
    split_group_exprs(&e->g);
    split_expr(e->body);
    int ng;
    Group *gs = scc(e->g.b, e->g.n, &ng);
    Expr *body = e->body, *cur = e;
    for (int i = 0; i < ng; i++) {
      if (i > 0) {
        Expr *n = NEW(Expr);
        *n = *e;
        cur->body = n;
        cur = n;
      }
      cur->g = gs[i];
    }
    cur->body = body;
    return;
  }
  case E_IF:
    split_expr(e->cond);
    split_expr(e->then_);
    split_expr(e->else_);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) split_expr(e->scruts[i]);
    for (int a = 0; a < e->na; a++) split_rhs(&e->alts[a]->rhs);
    return;
  case E_ANN: split_expr(e->body); return;
  default: return;
  }
}

/* ------------------------------------------------------------- inference */

typedef struct Env { const char *name; Type *t; struct Env *next; } Env;

static Env *env_push(Env *env, const char *name, Type *t) {
  Env *e = NEW(Env);
  e->name = name;
  e->t = t;
  e->next = env;
  return e;
}

static Type *infer(Expr *e, Env *env);
static void infer_group(Group *g, Env **env, bool top);

static int con_lookup(const char *name, int line, int col) {
  int id = map_get(&cmap, name);
  if (id < 0) cerror(line, col, "unknown constructor `%s`", name);
  return id;
}

static Type *infer_pat(Pat *p, Env **env, Env *stop) {
  switch (p->k) {
  case P_VAR: {
    for (Env *e = *env; e != stop; e = e->next)
      if (e->name == p->name) cerror(p->line, p->col, "variable `%s` is bound twice in one pattern", p->name);
    Type *t = newvar();
    *env = env_push(*env, p->name, t);
    return t;
  }
  case P_WILD: return newvar();
  case P_INT: return numvar();
  case P_CHAR: return tcon0(s_char);
  case P_CON: {
    int id = con_lookup(p->name, p->line, p->col);
    ConDef *c = &condefs.a[id];
    if (p->n != c->arity)
      cerror(p->line, p->col, "constructor `%s` takes %d argument%s, pattern has %d", p->name, c->arity,
             c->arity == 1 ? "" : "s", p->n);
    p->con = id;
    Type *t = inst(c->scheme, false);
    for (int i = 0; i < p->n; i++) {
      t = repr(t);
      Type *ta = infer_pat(p->args[i], env, stop);
      unify(t->args[0], ta, p->args[i]->line, p->args[i]->col);
      t = t->args[1];
    }
    return t;
  }
  }
  abort();
}

static void infer_rhs(Rhs *r, Env *env, Type *res) {
  for (int i = 0; i < r->nwg; i++) infer_group(&r->wg[i], &env, false);
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) unify(tcon0(s_bool), infer(r->guards[i], env), r->guards[i]->line, r->guards[i]->col);
    unify(res, infer(r->bodies[i], env), r->bodies[i]->line, r->bodies[i]->col);
  }
}

static Type *infer(Expr *e, Env *env) {
  switch (e->k) {
  case E_VAR: {
    for (Env *x = env; x; x = x->next)
      if (x->name == e->name) return inst(x->t, false);
    int g = map_get(&gmap, e->name);
    if (g < 0) cerror(e->line, e->col, "variable not in scope: `%s`", e->name);
    return inst(globals.a[g].type, false);
  }
  case E_CON:
    e->idx = con_lookup(e->name, e->line, e->col);
    return inst(condefs.a[e->idx].scheme, false);
  case E_INT: return e->ty = numvar();
  case E_FLOAT: return tcon0(s_float);
  case E_CHAR: return tcon0(s_char);
  case E_STR: return tlist(tcon0(s_char));
  case E_APP: {
    Type *tf = infer(e->fn, env);
    for (int i = 0; i < e->n; i++) {
      Type *ta = infer(e->args[i], env);
      Type *f = repr(tf);
      if (f->k == TY_CON && f->name == s_arrow) {
        unify(f->args[0], ta, e->args[i]->line, e->args[i]->col);
        tf = f->args[1];
      } else {
        Type *r = newvar();
        unify(f, tarrow(ta, r), e->args[i]->line, e->args[i]->col);
        tf = r;
      }
    }
    return tf;
  }
  case E_LAM: {
    Lambda *L = e->lam;
    Type **ps = xmalloc((size_t)L->n * sizeof *ps);
    for (int i = 0; i < L->n; i++) env = env_push(env, L->params[i], ps[i] = newvar());
    Type *t = infer(L->body, env);
    for (int i = L->n; i-- > 0;) t = tarrow(ps[i], t);
    free(ps);
    return t;
  }
  case E_LET:
    infer_group(&e->g, &env, false);
    return infer(e->body, env);
  case E_IF: {
    unify(tcon0(s_bool), infer(e->cond, env), e->cond->line, e->cond->col);
    Type *t = infer(e->then_, env);
    unify(t, infer(e->else_, env), e->else_->line, e->else_->col);
    return t;
  }
  case E_CASE: {
    Type **ts = xmalloc((size_t)e->ns * sizeof *ts + 1);
    for (int i = 0; i < e->ns; i++) ts[i] = infer(e->scruts[i], env);
    Type *res = newvar();
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      Env *ae = env;
      for (int i = 0; i < alt->np; i++)
        unify(ts[i], infer_pat(alt->pats[i], &ae, env), alt->pats[i]->line, alt->pats[i]->col);
      infer_rhs(&alt->rhs, ae, res);
    }
    free(ts);
    return res;
  }
  case E_ANN: {
    Type *t = infer(e->body, env);
    unify(inst(sig_scheme(e->sig), false), t, e->line, e->col);
    return t;
  }
  }
  abort();
}

static void infer_group(Group *g, Env **env, bool top) {
  Env *none = NULL;
  if (!env) env = &none;
  level++;
  Type **mono = xmalloc((size_t)g->n * sizeof *mono);
  Type **sch = xmalloc((size_t)g->n * sizeof *sch);
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    sch[i] = b->sig ? sig_scheme(b->sig) : NULL;
    mono[i] = sch[i] ? sch[i] : newvar();
    if (top) globals.a[b->gidx].type = mono[i];
    else *env = env_push(*env, b->name, mono[i]);
  }
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    Type *t = infer(b->e, *env);
    if (sch[i]) unify(inst(sch[i], true), t, b->line, b->col);
    else unify(mono[i], t, b->line, b->col);
  }
  level--;
  for (int i = 0; i < g->n; i++)
    if (!sch[i]) gen(mono[i], g->b[i]->e->k != E_LAM);
  free(mono);
  free(sch);
}

static void default_nums(void) {
  for (int i = 0; i < numvars.n; i++) {
    Type *r = repr(numvars.a[i]);
    if (r->k == TY_VAR && !r->skolem && r->level != GENERIC) r->link = tcon0(s_int);
  }
  numvars.n = 0;
}

/* ---------------------------------------------------------------- driver */

static void add_tydef(const char *name, int arity, int ncons) {
  VPUSH(tydefs, ((TyDef){name, arity, ncons}));
  map_put(&tmap, name, tydefs.n - 1);
}

static void add_con(const char *name, Type *scheme, int arity, const char *ty) {
  VPUSH(condefs, ((ConDef){name, arity, scheme, map_get(&tmap, ty)}));
  map_put(&cmap, name, condefs.n - 1);
}

void compiler_init(void) {
  s_int = intern("Int");
  s_float = intern("Float");
  s_char = intern("Char");
  s_bool = intern("Bool");
  s_list = intern("[]");
  s_arrow = intern("->");
  s_unit = intern("()");
  s_string = intern("String");
  static const char *tuples[] = {"(,)", "(,,)", "(,,,)", "(,,,,)", "(,,,,,)", "(,,,,,,)"};
  add_tydef(s_int, 0, 0);
  add_tydef(s_float, 0, 0);
  add_tydef(s_char, 0, 0);
  add_tydef(s_bool, 0, 2);
  add_tydef(s_unit, 0, 1);
  add_tydef(s_list, 1, 2);
  add_tydef(s_arrow, 2, 0);
  add_tydef(intern("Num"), 1, 0); /* reserved; index < 8 marks builtins */
  for (int i = 0; i < FK_MAXTUPLE - 1; i++) add_tydef(intern(tuples[i]), i + 2, 1);

  fk_init();
  Type *bool_t = tcon0(s_bool);
  add_con(intern("False"), bool_t, 0, s_bool);
  add_con(intern("True"), bool_t, 0, s_bool);
  add_con(s_unit, tcon0(s_unit), 0, s_unit);
  Type *a = gvar(false);
  add_con(s_list, tlist(a), 0, s_list);
  add_con(intern(":"), tarrow(a, tarrow(tlist(a), tlist(a))), 2, s_list);
  for (int i = 0; i < FK_MAXTUPLE - 1; i++) {
    int n = i + 2;
    Type *vs[FK_MAXTUPLE];
    for (int j = 0; j < n; j++) vs[j] = gvar(false);
    Type *t = tcon(intern(tuples[i]), n, vs);
    for (int j = n; j-- > 0;) t = tarrow(vs[j], t);
    add_con(intern(tuples[i]), t, n, tuples[i]);
  }
  for (int i = 0; i < fk_nprims; i++) {
    Global g = {intern(fk_prims[i].name), sig_scheme(parse_sig_text(prim_info[i].type)), i, NULL};
    VPUSH(globals, g);
    map_put(&gmap, g.name, globals.n - 1);
  }
}

bool compile_chunk(Module *m, Chunk *out) {
  jmp_buf jb, *saved = cerr_jmp;
  int mark = undo_mark(), ng0 = globals.n, nc0 = condefs.n, nt0 = tydefs.n;
  numvars.n = 0;
  level = 0;
  if (setjmp(jb)) {
    cerr_jmp = saved;
    undo_to(mark);
    globals.n = ng0;
    condefs.n = nc0;
    tydefs.n = nt0;
    fk_truncate_cons((uint32_t)nc0);
    numvars.n = 0;
    level = 0;
    return false;
  }
  cerr_jmp = &jb;
  register_datas(m);
  for (int i = 0; i < m->nb; i++) {
    Bind *b = m->binds[i];
    if (b->e->k == E_LAM) b->e->lam->name = b->name;
    b->gidx = globals.n;
    VPUSH(globals, ((Global){b->name, NULL, -1, b->e->k == E_LAM ? b->e->lam : NULL}));
    map_put(&gmap, b->name, b->gidx);
  }
  Group tmp = {m->binds, m->nb, false};
  split_group_exprs(&tmp);
  int ng;
  Group *gs = scc(m->binds, m->nb, &ng);
  for (int i = 0; i < ng; i++) infer_group(&gs[i], NULL, true);
  default_nums();
  /* Evaluate `main` last, unless another binding uses it. */
  for (int i = 0; i < ng; i++) {
    if (gs[i].n != 1 || strcmp(gs[i].b[0]->name, "main") != 0) continue;
    Deps d = {gs[i].b, 1, 0, (bool[1]){false}};
    for (int j = 0; j < m->nb; j++)
      if (m->binds[j] != gs[i].b[0]) deps_expr(m->binds[j]->e, NULL, &d);
    if (!d.adj[0]) {
      Group g = gs[i];
      memmove(&gs[i], &gs[i + 1], (size_t)(ng - i - 1) * sizeof *gs);
      gs[ng - 1] = g;
    }
    break;
  }
  for (int i = 0; i < m->nb; i++) resolve_top(m->binds[i]);
  out->groups = gs;
  out->ng = ng;
  cerr_jmp = saved;
  return true;
}

const char *infer_type_text(Expr *e) {
  jmp_buf jb, *saved = cerr_jmp;
  if (setjmp(jb)) {
    cerr_jmp = saved;
    numvars.n = 0;
    level = 0;
    return NULL;
  }
  cerr_jmp = &jb;
  numvars.n = 0;
  split_expr(e);
  level = 1;
  Type *t = infer(e, NULL);
  level = 0;
  gen(t, false);
  numvars.n = 0;
  cerr_jmp = saved;
  return type_str(t);
}
