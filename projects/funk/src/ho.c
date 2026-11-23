/* ho.c -- specialization on function arguments, after dictionary passing (elab.h).

   After elaboration, a call to a top-level function whose argument for a
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
#include "elab.h"

#define HO_MAX_COPIES 4000 /* per chunk */

/* Can e be substituted into a copy? Its variables must be globals, its
   own lambda parameters (inner), or locals of the call site (outer), which
   are collected in fv. */
static bool liftable(Expr *e, Nm *inner, Nm *outer, int depth, Fv *fv) {
  if (depth > 6) return false;
  switch (e->k) {
  case E_VAR:
    if (el_in(inner, e->name)) return true;
    if (!e->global_ref && el_in(outer, e->name)) {
      if (el_fv_index(fv, e->name) < 0) {
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
    for (int i = 0; i < e->lam->n; i++) inner = el_nm(inner, e->lam->params[i]);
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
      for (int i = 0; i < alt->np; i++) t = el_pat_nm(alt->pats[i], t);
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
  case P_VAR: *params = el_nm(*params, p->name); sb_puts(b, "v"); return;
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
/* A global's name in a copy's key; a redefined one's with its index, so
   that a copy made from the old definition is not reused for the new. */
static void key_global(SB *b, const char *name) {
  int g = global_lookup(name);
  if (g >= 0 && CS->globals.a[g].redefined) sb_printf(b, "%s#%d", name, g);
  else sb_puts(b, name);
}

static void key(SB *b, Expr *e, Nm *params) {
  switch (e->k) {
  case E_VAR: {
    int i = 0;
    for (Nm *s = params; s; s = s->next, i++)
      if (s->name == e->name) {
        sb_printf(b, "#%d", i);
        return;
      }
    if (!e->global_ref && (i = el_fv_index(ST->key_fv, e->name)) >= 0) {
      sb_printf(b, "$%d", i);
      return;
    }
    key_global(b, e->name);
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
    for (int i = 0; i < e->lam->n; i++) params = el_nm(params, e->lam->params[i]);
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
  default: BUG();
  }
}

/* Pin the global references of a copied argument. Variables in inner (its
   lambda parameters, and the copy's leading parameters) stay local. */
static void pin(Expr *e, Nm *inner) {
  switch (e->k) {
  case E_VAR: if (!el_in(inner, e->name)) e->global_ref = true; return;
  case E_APP:
    pin(e->fn, inner);
    for (int i = 0; i < e->n; i++) pin(e->args[i], inner);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) inner = el_nm(inner, e->lam->params[i]);
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
      for (int i = 0; i < alt->np; i++) t = el_pat_nm(alt->pats[i], t);
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
    if (!el_in(inner, e->name) && !e->global_ref && (i = el_fv_index(fv, e->name)) >= 0) e->name = to[i];
    return;
  }
  case E_APP:
    rename_fv(e->fn, fv, to, inner);
    for (int i = 0; i < e->n; i++) rename_fv(e->args[i], fv, to, inner);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) inner = el_nm(inner, e->lam->params[i]);
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
      for (int i = 0; i < alt->np; i++) t = el_pat_nm(alt->pats[i], t);
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
    if (el_in(el_pat_nm(alt->pats[i], NULL), x)) return;
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
    if (el_ground_depth(t) >= 0 && el_ground_depth(t) <= 4) VPUSH(*out, ((TS){pat, t}));
    return;
  }
  if (t->k != TY_CON || pat->name != t->name || pat->n != t->n) return;
  for (int i = 0; i < pat->n; i++) match_type(pat->args[i], t->args[i], out);
}

/* Specialize the call e if it qualifies; locals are the call site's. */
static void ho_call(Expr *e, Nm *locals) {
  Expr *f = e->fn;
  if (f->k != E_VAR || (!f->global_ref && el_in(locals, f->name))) return;
  int g = global_lookup(f->name);
  Bind *fb = g >= 0 ? CS->globals.a[g].bind : NULL;
  if (!fb || fb->e->k != E_LAM || !fb->e->lam->ptypes) return;
  if (ST->nho >= HO_MAX_COPIES) {
    if (!ST->ho_capped) opt_note(e->line, "not specialized: %s and later calls; the input made %d copies", f->name, HO_MAX_COPIES);
    ST->ho_capped = true;
    return;
  }
  Lambda *L = fb->e->lam;
  int m = e->n < L->n ? e->n : L->n;
  bool *use = xcalloc((size_t)m + 1, sizeof *use);
  Fv fv = {0};
  int nuse = 0;
  for (int i = 0; i < m; i++) {
    Expr *x = e->args[i];
    if (!fun_type(L->ptypes[i]) || (x->k == E_VAR && !x->global_ref && el_in(locals, x->name))) continue;
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
  if (CS->globals.a[g].redefined) sb_printf(&k, "%s#%d@{", fb->name, g);
  else sb_printf(&k, "%s@{", fb->name);
  for (int i = 0, c = 0; i < m; i++) {
    if (!use[i]) continue;
    SB a = {0};
    sb_puts(&a, "");
    key(&a, e->args[i], NULL);
    if (a.n > 160) return; /* a growing argument: stop specializing */
    sb_printf(&k, "%s%d=%s", c++ ? ";" : "", i, a.s);
  }
  sb_puts(&k, "}");
  if (k.n > 512) return; /* a copy of a copy of ...: each argument is short, the chain is not */
  if (ts.n) { /* the types are part of the copy: `map id` differs at [Int] and [Char] */
    TS *saved = ST->tsub;
    int nsaved = ST->ntsub;
    ST->tsub = ts.a;
    ST->ntsub = ts.n;
    sb_puts(&k, "<");
    for (int i = 0; i < L->n; i++) sb_printf(&k, "%s%s", i ? "," : "", L->ptypes[i] ? type_str(el_apply(L->ptypes[i])) : "$");
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
      el_subst_types(b->e);
      b->type = el_apply(b->type);
      ST->tsub = saved;
      ST->ntsub = nsaved;
    }
    Lambda *C = b->e->lam;
    const char **hp = xmalloc((size_t)nf * sizeof *hp + 1);
    Nm *keep = NULL;
    for (int i = 0; i < nf; i++) keep = el_nm(keep, hp[i] = intern(xsprintf("$h%d", ST->nhparam++)));
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
    Expr *v = el_var(fv.names.a[i], f->line, f->col);
    v->ty = fv.types.a[i];
    rest[n++] = v;
  }
  for (int i = 0; i < e->n; i++)
    if (!(i < m && use[i])) rest[n++] = e->args[i];
  Expr *v = el_var(name, f->line, f->col);
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
  for (int i = 0; i < g->n; i++) s = el_nm(s, g->b[i]->name);
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
    Expr *v = el_mk(E_VAR, e->line, e->col);
    v->name = fresh[i];
    v->ty = L->ptypes ? L->ptypes[i] : NULL;
    Nm *saved = ST->keep_locals;
    ST->keep_locals = el_nm(NULL, fresh[i]); /* the fresh name stays local */
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
    Expr *c = el_mk(E_CASE, e->line, e->col);
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
    for (int i = 0; i < e->lam->n; i++) s = el_nm(s, e->lam->params[i]);
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
      for (int i = 0; i < alt->np; i++) t = el_pat_nm(alt->pats[i], t);
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


void ho_begin(void) {
  ST->nho = 0;
  ST->ho_capped = false;
}

void ho_specialize(Bind **bs, int n) {
  for (int i = 0; i < n; i++) VPUSH(ST->ho_work, bs[i]);
  while (ST->ho_work.n) {
    Bind *b = ST->ho_work.a[--ST->ho_work.n];
    ho_expr(b->e, NULL);
  }
}
