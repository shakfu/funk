/* cgen.c -- emit a standalone C program: the runtime source, then one C
   function per lambda, then an initializer for the globals.

   Frame slots become C locals l<i>, free variables are read from the
   closure. Saturated calls to known lambdas, primitives and constructors
   are direct; anything else goes through fk_apply. Values of scalar type
   are C scalars inside a function body; function boundaries, data fields
   and closures use V (typed-backend.md). A self call in tail
   position becomes a jump back to the top of the function. Arguments are
   evaluated left to right into temporaries before each call, since C
   leaves argument order unspecified. */
#include "funk.h"

const char *prim_cname(int i);

/* A C representation. Monomorphic values of scalar type live in C scalars
   (typed-backend.md); everything else is a tagged word V. */
typedef enum { R_V, R_I, R_F, R_C, R_B } Rep;
typedef struct { const char *s; Rep r; } TV; /* a C expression and its representation */

static const char *const ctype[] = {"V", "int64_t", "double", "int32_t", "bool"};

typedef struct {
  SB *out;
  Lambda *lam;
  int tmp, label;
  bool looped;
  Rep *slot; /* representation of each frame slot */
} CG;

static TV cg_expr(CG *c, Expr *e);
static void cg_tail(CG *c, Expr *e);

static Rep rep_of(Type *t) {
  if (!t) return R_V;
  t = repr(t);
  if (t->k != TY_CON || t->n) return R_V;
  if (!strcmp(t->name, "Int")) return R_I;
  if (!strcmp(t->name, "Float")) return R_F;
  if (!strcmp(t->name, "Char")) return R_C;
  if (!strcmp(t->name, "Bool")) return R_B;
  return R_V;
}

/* v converted to representation to; boxing a Float allocates. */
static const char *conv(TV v, Rep to) {
  if (v.r == to) return v.s;
  if (v.r != R_V && to != R_V) return conv((TV){conv(v, R_V), R_V}, to);
  if (to == R_V) {
    static const char *const box[] = {NULL, "FK_MKINT(%s)", "fk_float(%s)", "FK_MKCHAR(%s)", "FK_BOOL(%s)"};
    return xsprintf(box[v.r], v.s);
  }
  static const char *const unbox[] = {NULL, "FK_INT(%s)", "FK_FLOATV(%s)", "FK_CHAR(%s)", "FK_ISTRUE(%s)"};
  return xsprintf(unbox[to], v.s);
}

static TV tv(const char *s, Rep r) { return (TV){s, r}; }
static const char *as_v(TV v) { return conv(v, R_V); }

static const char *newtmp(CG *c) { return xsprintf("t%d", c->tmp++); }

/* A new temporary holding v, so its evaluation happens here. */
static TV hold(CG *c, TV v) {
  const char *t = newtmp(c);
  sb_printf(c->out, "  %s %s = %s;\n", ctype[v.r], t, v.s);
  return tv(t, v.r);
}

static TV var_ref(CG *c, Expr *e) {
  switch (e->vk) {
  case VK_LOCAL: return tv(xsprintf("l%d", e->idx), c->slot[e->idx]);
  case VK_FREE: return tv(xsprintf("FK_FREE(self, %d)", e->idx), R_V);
  default: return tv(xsprintf("G[%d]", e->idx), R_V);
  }
}

static const char *src_ref(CG *c, FreeSrc s) {
  if (s.vk == VK_LOCAL) return as_v(tv(xsprintf("l%d", s.idx), c->slot[s.idx]));
  return xsprintf("FK_FREE(self, %d)", s.idx);
}

static void c_string(SB *b, const char *s, int n) {
  sb_puts(b, "\"");
  for (int i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch >= 32 && ch < 127 && ch != '"' && ch != '\\' && ch != '?') sb_printf(b, "%c", ch);
    else sb_printf(b, "\\%03o", ch);
  }
  sb_puts(b, "\"");
}

static const char *arg_array(const char **a, int n) {
  SB b = {0};
  sb_puts(&b, "(V[]){");
  for (int i = 0; i < n; i++) sb_printf(&b, "%s%s", i ? ", " : "", a[i]);
  sb_puts(&b, "}");
  return b.s;
}

static const char *closure_new(CG *c, Lambda *L) {
  const char *t = newtmp(c);
  sb_printf(c->out, "  V %s = fk_closure(fn_%d, %d, %d, 0);\n", t, L->id, L->n, L->nfree);
  return t;
}

static void closure_fill(CG *c, const char *v, Lambda *L) {
  for (int i = 0; i < L->nfree; i++) sb_printf(c->out, "  FK_FREE(%s, %d) = %s;\n", v, i, src_ref(c, L->free[i]));
}

static void cg_group(CG *c, Group *g) {
  if (!g->rec) {
    Bind *b = g->b[0];
    if (b->thunk) {
      const char *v = newtmp(c);
      sb_printf(c->out, "  V %s = fk_thunk(fn_%d, %d, 0);\n", v, b->thunk->id, b->thunk->nfree);
      closure_fill(c, v, b->thunk);
      sb_printf(c->out, "  l%d = %s;\n", b->slot, v);
    } else {
      sb_printf(c->out, "  l%d = %s;\n", b->slot, conv(cg_expr(c, b->e), c->slot[b->slot]));
    }
    return;
  }
  for (int i = 0; i < g->n; i++)
    sb_printf(c->out, "  l%d = %s;\n", g->b[i]->slot, closure_new(c, g->b[i]->e->lam));
  for (int i = 0; i < g->n; i++) closure_fill(c, xsprintf("l%d", g->b[i]->slot), g->b[i]->e->lam);
}

/* Primitives on C scalars. An argument representation of -1 takes the
   argument's own: == is one primitive for Int and Char. */
typedef struct { const char *name, *fmt; int nargs; int arg; Rep res; } Scalar;
static const Scalar scalars[] = {
    {"addi", "(int64_t)((uint64_t)%s + (uint64_t)%s)", 2, R_I, R_I},
    {"subi", "(int64_t)((uint64_t)%s - (uint64_t)%s)", 2, R_I, R_I},
    {"muli", "(int64_t)((uint64_t)%s * (uint64_t)%s)", 2, R_I, R_I},
    {"negi", "(int64_t)(0 - (uint64_t)%s)", 1, R_I, R_I},
    {"absi", "fk_s_absi(%s)", 1, R_I, R_I},
    {"div", "fk_s_div(%s, %s)", 2, R_I, R_I},
    {"mod", "fk_s_mod(%s, %s)", 2, R_I, R_I},
    {"quot", "fk_s_quot(%s, %s)", 2, R_I, R_I},
    {"rem", "fk_s_rem(%s, %s)", 2, R_I, R_I},
    {"addf", "(%s + %s)", 2, R_F, R_F},
    {"subf", "(%s - %s)", 2, R_F, R_F},
    {"mulf", "(%s * %s)", 2, R_F, R_F},
    {"fdiv", "(%s / %s)", 2, R_F, R_F},
    {"negf", "(-%s)", 1, R_F, R_F},
    {"absf", "fabs(%s)", 1, R_F, R_F},
    {"sqrt", "sqrt(%s)", 1, R_F, R_F},
    {"exp", "exp(%s)", 1, R_F, R_F},
    {"log", "log(%s)", 1, R_F, R_F},
    {"sin", "sin(%s)", 1, R_F, R_F},
    {"cos", "cos(%s)", 1, R_F, R_F},
    {"pow", "pow(%s, %s)", 2, R_F, R_F},
    {"tofloat", "((double)%s)", 1, R_I, R_F},
    {"ord", "((int64_t)%s)", 1, R_C, R_I},
    {"chr", "fk_s_chr(%s)", 1, R_I, R_C},
    {"eqw", "(%s == %s)", 2, -1, R_B},
    {"new", "(%s != %s)", 2, -1, R_B},
    {"ltw", "(%s < %s)", 2, -1, R_B},
    {"lew", "(%s <= %s)", 2, -1, R_B},
    {"gtw", "(%s > %s)", 2, -1, R_B},
    {"gew", "(%s >= %s)", 2, -1, R_B},
    {"eqf", "(%s == %s)", 2, R_F, R_B},
    {"nef", "(%s != %s)", 2, R_F, R_B},
    {"ltf", "(%s < %s)", 2, R_F, R_B},
    {"lef", "(%s <= %s)", 2, R_F, R_B},
    {"gtf", "(%s > %s)", 2, R_F, R_B},
    {"gef", "(%s >= %s)", 2, R_F, R_B},
};

static const Scalar *scalar_prim(const char *c) {
  for (size_t i = 0; i < sizeof scalars / sizeof scalars[0]; i++)
    if (strcmp(scalars[i].name, c) == 0) return &scalars[i];
  return NULL;
}

/* The call for an application, as a C expression; args evaluated first,
   left to right. */
static TV cg_call(CG *c, Expr *e) {
  Expr *f = e->fn;
  int n = e->n;
  int g = f->k == E_VAR && f->vk == VK_GLOBAL ? f->idx : -1;
  int prim = g >= 0 ? globals.a[g].prim : -1;
  const Scalar *sc = prim >= 0 && n == fk_prims[prim].arity ? scalar_prim(prim_cname(prim)) : NULL;
  if (sc) {
    TV x[2];
    for (int i = 0; i < n; i++) x[i] = cg_expr(c, e->args[i]);
    Rep ar = sc->arg >= 0 ? (Rep)sc->arg : rep_of(e->args[0]->ty);
    if (ar == R_V) ar = rep_of(e->args[1 % n]->ty); /* an untyped first argument */
    const char *a0 = conv(x[0], ar), *a1 = n > 1 ? conv(x[1], ar) : NULL;
    return tv(n == 1 ? xsprintf(sc->fmt, a0) : xsprintf(sc->fmt, a0, a1), sc->res);
  }
  bool direct = f->k == E_CON || prim >= 0 || (f->k == E_VAR && f->known);
  const char *fv = direct ? NULL : as_v(cg_expr(c, f));
  const char **a = xmalloc((size_t)n * sizeof *a);
  for (int i = 0; i < n; i++) a[i] = as_v(cg_expr(c, e->args[i]));
  int arity;
  const char *call;
  if (f->k == E_CON) {
    arity = fk_cons[f->idx].arity;
    if (n < arity) return tv(xsprintf("fk_apply(fk_con_value(%d), %d, %s)", f->idx, n, arg_array(a, n)), R_V);
    call = xsprintf("fk_mkcon(%d, %d, %s)", f->idx, n, arg_array(a, n));
  } else if (prim >= 0) {
    arity = fk_prims[prim].arity;
    if (n < arity) return tv(xsprintf("fk_apply(G[%d], %d, %s)", g, n, arg_array(a, n)), R_V);
    call = xsprintf("fk_p_%s(0, %s)", prim_cname(prim), arg_array(a, arity));
  } else if (direct) {
    arity = f->known->n;
    const char *self = as_v(var_ref(c, f));
    if (n < arity) return tv(xsprintf("fk_apply(%s, %d, %s)", self, n, arg_array(a, n)), R_V);
    call = xsprintf("fn_%d(%s, %s)", f->known->id, self, arg_array(a, arity));
  } else {
    return tv(xsprintf("fk_apply(%s, %d, %s)", fv, n, arg_array(a, n)), R_V);
  }
  if (n > arity) call = xsprintf("fk_apply(%s, %d, %s)", call, n - arity, arg_array(a + arity, n - arity));
  return tv(call, R_V);
}

/* Tests of pattern p against path, whose representation is r; fields are V. */
static void pat_cond(SB *cond, Pat *p, const char *path, Rep r) {
  switch (p->k) {
  case P_VAR:
  case P_WILD: return;
  case P_INT:
    if (r == R_I) sb_printf(cond, " && %s == %lldLL", path, p->ival);
    else if (r == R_F) sb_printf(cond, " && %s == %lld.0", path, p->ival);
    else sb_printf(cond, " && fk_num_eq_int(%s, %lldLL)", path, p->ival);
    return;
  case P_CHAR:
    if (r == R_C) sb_printf(cond, " && %s == %lld", path, p->ival);
    else sb_printf(cond, " && %s == FK_MKCHAR(%lld)", path, p->ival);
    return;
  default:
    if (r == R_B) {
      sb_printf(cond, p->con == FK_C_TRUE ? " && %s" : " && !%s", path);
      return;
    }
    if (tydefs.a[condefs.a[p->con].tydef].ncons > 1) sb_printf(cond, " && FK_CONID(%s) == %d", path, p->con);
    for (int i = 0; i < p->n; i++) pat_cond(cond, p->args[i], xsprintf("FK_FIELD(%s, %d)", path, i), R_V);
  }
}

static void pat_bind(CG *c, Pat *p, const char *path, Rep r) {
  if (p->k == P_VAR) sb_printf(c->out, "  l%d = %s;\n", p->slot, conv(tv(path, r), c->slot[p->slot]));
  for (int i = 0; p->k == P_CON && r == R_V && i < p->n; i++)
    pat_bind(c, p->args[i], xsprintf("FK_FIELD(%s, %d)", path, i), R_V);
}

/* Alternatives are tried in order; a failed pattern or a fallthrough
   past every guard jumps to the next one. */
static TV cg_case(CG *c, Expr *e, bool tail) {
  TV *sv = xmalloc((size_t)e->ns * sizeof *sv + 1);
  for (int i = 0; i < e->ns; i++) {
    Rep r = rep_of(e->scruts[i]->ty);
    sv[i] = hold(c, tv(conv(cg_expr(c, e->scruts[i]), r), r));
  }
  Rep rr = rep_of(e->ty);
  const char *r = NULL;
  int done = c->label++;
  if (!tail) {
    r = newtmp(c);
    sb_printf(c->out, "  %s %s = 0;\n", ctype[rr], r);
  }
  for (int a = 0; a < e->na; a++) {
    Alt *alt = e->alts[a];
    int next = c->label++;
    SB cond = {0};
    sb_puts(&cond, "");
    for (int i = 0; i < alt->np; i++) pat_cond(&cond, alt->pats[i], sv[i].s, sv[i].r);
    if (cond.n) sb_printf(c->out, "  if (!(1%s)) goto L%d;\n", cond.s, next);
    sb_puts(c->out, "  {\n");
    for (int i = 0; i < alt->np; i++) pat_bind(c, alt->pats[i], sv[i].s, sv[i].r);
    Rhs *rh = &alt->rhs;
    for (int i = 0; i < rh->nwg; i++) cg_group(c, &rh->wg[i]);
    bool total = false;
    for (int i = 0; i < rh->ng && !total; i++) {
      if (rh->guards[i]) {
        sb_printf(c->out, "  if (%s) {\n", conv(cg_expr(c, rh->guards[i]), R_B));
      } else {
        total = true;
        sb_puts(c->out, "  {\n");
      }
      if (tail) {
        cg_tail(c, rh->bodies[i]);
      } else {
        const char *v = conv(cg_expr(c, rh->bodies[i]), rr);
        sb_printf(c->out, "  %s = %s; goto L%d;\n", r, v, done);
      }
      sb_puts(c->out, "  }\n");
    }
    sb_printf(c->out, "  }\n  L%d: ;\n", next);
  }
  SB where = {0};
  const char *loc = xsprintf("%s:%d:%d", e->name, e->line, e->col);
  c_string(&where, loc, (int)strlen(loc));
  sb_printf(c->out, "  fk_match_fail(%s);\n", where.s);
  if (!tail) sb_printf(c->out, "  L%d: ;\n", done);
  return tv(r, rr);
}

static TV cg_expr(CG *c, Expr *e) {
  const char *t;
  switch (e->k) {
  case E_VAR:
    if (!e->lazy) return var_ref(c, e);
    t = newtmp(c); /* a temporary keeps forcing in evaluation order */
    sb_printf(c->out, "  V %s = fk_force(%s);\n", t, as_v(var_ref(c, e)));
    return tv(t, R_V);
  case E_CON:
    if (e->idx == FK_C_TRUE || e->idx == FK_C_FALSE) return tv(e->idx == FK_C_TRUE ? "1" : "0", R_B);
    return tv(xsprintf("fk_con_value(%d)", e->idx), R_V);
  case E_INT:
    if (rep_of(e->ty) == R_I) return tv(xsprintf("%lldLL", e->ival), R_I);
    return tv(xsprintf("FK_MKINT(%lldLL)", e->ival), R_V);
  case E_CHAR: return tv(xsprintf("%lld", e->ival), R_C);
  case E_FLOAT: return tv(xsprintf("((double)%.17g)", e->fval), R_F); /* 0.0 must not print as the int 0 */
  case E_STR: {
    SB s = {0};
    c_string(&s, e->name, e->slen);
    t = newtmp(c);
    sb_printf(c->out, "  V %s = fk_string(%s, %d);\n", t, s.s, e->slen);
    return tv(t, R_V);
  }
  case E_APP: return hold(c, cg_call(c, e));
  case E_LAM:
    t = closure_new(c, e->lam);
    closure_fill(c, t, e->lam);
    return tv(t, R_V);
  case E_LET: cg_group(c, &e->g); return cg_expr(c, e->body);
  case E_IF: {
    const char *cond = conv(cg_expr(c, e->cond), R_B);
    Rep r = rep_of(e->ty);
    t = newtmp(c);
    sb_printf(c->out, "  %s %s;\n  if (%s) {\n", ctype[r], t, cond);
    sb_printf(c->out, "  %s = %s;\n  } else {\n", t, conv(cg_expr(c, e->then_), r));
    sb_printf(c->out, "  %s = %s;\n  }\n", t, conv(cg_expr(c, e->else_), r));
    return tv(t, r);
  }
  case E_CASE: return cg_case(c, e, false);
  case E_ANN: return cg_expr(c, e->body);
  default: abort();
  }
}

static void cg_tail(CG *c, Expr *e) {
  switch (e->k) {
  case E_APP: {
    Expr *f = e->fn;
    if (f->k == E_VAR && f->known == c->lam && c->lam->n == e->n) {
      const char **a = xmalloc((size_t)e->n * sizeof *a);
      for (int i = 0; i < e->n; i++) a[i] = conv(cg_expr(c, e->args[i]), c->slot[i]);
      int base = c->tmp;
      c->tmp += e->n;
      for (int i = 0; i < e->n; i++) sb_printf(c->out, "  %s t%d = %s;\n", ctype[c->slot[i]], base + i, a[i]);
      for (int i = 0; i < e->n; i++) sb_printf(c->out, "  l%d = t%d;\n", i, base + i);
      sb_puts(c->out, "  goto top;\n");
      c->looped = true;
      return;
    }
    sb_printf(c->out, "  return %s;\n", as_v(cg_call(c, e)));
    return;
  }
  case E_LET: cg_group(c, &e->g); cg_tail(c, e->body); return;
  case E_IF: {
    sb_printf(c->out, "  if (%s) {\n", conv(cg_expr(c, e->cond), R_B));
    cg_tail(c, e->then_);
    sb_puts(c->out, "  } else {\n");
    cg_tail(c, e->else_);
    sb_puts(c->out, "  }\n");
    return;
  }
  case E_CASE: cg_case(c, e, true); return;
  case E_ANN: cg_tail(c, e->body); return;
  default: sb_printf(c->out, "  return %s;\n", as_v(cg_expr(c, e))); return;
  }
}

/* Slot representations of a lambda: parameters, and the let, where and
   pattern variables in its body. Nested lambdas and thunk bodies are
   separate functions with their own slots. */
static void slots_expr(Rep *s, Expr *e);

static void slots_pat(Rep *s, Pat *p) {
  if (p->k == P_VAR) s[p->slot] = rep_of(p->ty);
  for (int i = 0; i < p->n; i++) slots_pat(s, p->args[i]);
}

static void slots_group(Rep *s, Group *g) {
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    s[b->slot] = g->rec || b->thunk ? R_V : rep_of(b->type);
    if (!b->thunk) slots_expr(s, b->e);
  }
}

static void slots_expr(Rep *s, Expr *e) {
  switch (e->k) {
  case E_APP:
    slots_expr(s, e->fn);
    for (int i = 0; i < e->n; i++) slots_expr(s, e->args[i]);
    return;
  case E_LET:
    slots_group(s, &e->g);
    slots_expr(s, e->body);
    return;
  case E_IF:
    slots_expr(s, e->cond);
    slots_expr(s, e->then_);
    slots_expr(s, e->else_);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) slots_expr(s, e->scruts[i]);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      for (int i = 0; i < alt->np; i++) slots_pat(s, alt->pats[i]);
      for (int i = 0; i < alt->rhs.nwg; i++) slots_group(s, &alt->rhs.wg[i]);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) slots_expr(s, alt->rhs.guards[i]);
        slots_expr(s, alt->rhs.bodies[i]);
      }
    }
    return;
  case E_ANN: slots_expr(s, e->body); return;
  default: return;
  }
}

static void cg_lambda(SB *out, Lambda *L) {
  Rep *slot = xcalloc((size_t)L->nslots + 1, sizeof *slot);
  for (int i = 0; i < L->n; i++) slot[i] = L->ptypes ? rep_of(L->ptypes[i]) : R_V;
  slots_expr(slot, L->body);
  SB body = {0};
  sb_puts(&body, "");
  CG c = {&body, L, 0, 0, false, slot};
  cg_tail(&c, L->body);
  char *nm = xsprintf("%s", L->name ? L->name : "lambda");
  for (char *s = nm; (s = strstr(s, "*/"));) s[1] = '|'; /* keep the comment closed */
  sb_printf(out, "static V fn_%d(V self, V *a) { /* %s */\n", L->id, nm);
  for (int i = 0; i < L->nslots; i++)
    sb_printf(out, "  %s l%d = %s;\n", ctype[slot[i]], i, i < L->n ? conv(tv(xsprintf("a[%d]", i), R_V), slot[i]) : "0");
  if (c.looped) sb_puts(out, "  top:;\n");
  sb_puts(out, body.s);
  sb_puts(out, "}\n\n");
}

/* Pass 0 creates function closures, pass 1 runs value initializers. Raw
   values (dictionaries) come before group values within pass 1, since
   init_global is called for them first. */
static void init_global(SB *out, Bind *b, int pass) {
  if (!pass && !b->init)
    sb_printf(out, "  G[%d] = fk_closure(fn_%d, %d, 0, 0);\n", b->gidx, b->e->lam->id, b->e->lam->n);
  if (pass && b->init) sb_printf(out, "  G[%d] = fn_%d(0, 0);\n", b->gidx, b->init->id);
}

/* ------------------------------------------------------- reachability */

/* Only code reachable from the program's roots is emitted: its top-level
   values, `main` among them, which run at startup whether used or not. */
typedef struct { Bind **bind; bool *global, *lam; } Reach;

static void reach_lambda(Reach *r, Lambda *L);

static void reach_global(Reach *r, int g) {
  if (r->global[g]) return;
  r->global[g] = true;
  Bind *b = r->bind[g];
  if (b) reach_lambda(r, b->init ? b->init : b->e->lam);
}

static void reach_expr(Reach *r, Expr *e);

static void reach_group(Reach *r, Group *g) {
  for (int i = 0; i < g->n; i++) {
    if (g->b[i]->thunk) reach_lambda(r, g->b[i]->thunk);
    else reach_expr(r, g->b[i]->e);
  }
}

static void reach_expr(Reach *r, Expr *e) {
  switch (e->k) {
  case E_VAR: if (e->vk == VK_GLOBAL) reach_global(r, e->idx); return;
  case E_APP:
    reach_expr(r, e->fn);
    for (int i = 0; i < e->n; i++) reach_expr(r, e->args[i]);
    return;
  case E_LAM: reach_lambda(r, e->lam); return;
  case E_LET:
    reach_group(r, &e->g);
    reach_expr(r, e->body);
    return;
  case E_IF:
    reach_expr(r, e->cond);
    reach_expr(r, e->then_);
    reach_expr(r, e->else_);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) reach_expr(r, e->scruts[i]);
    for (int a = 0; a < e->na; a++) {
      Rhs *rh = &e->alts[a]->rhs;
      for (int i = 0; i < rh->nwg; i++) reach_group(r, &rh->wg[i]);
      for (int i = 0; i < rh->ng; i++) {
        if (rh->guards[i]) reach_expr(r, rh->guards[i]);
        reach_expr(r, rh->bodies[i]);
      }
    }
    return;
  case E_ANN: reach_expr(r, e->body); return;
  default: return;
  }
}

static void reach_lambda(Reach *r, Lambda *L) {
  if (r->lam[L->id]) return;
  r->lam[L->id] = true;
  reach_expr(r, L->body);
}

static Reach reach_program(Chunk *chunks, int nchunks) {
  Reach r = {xcalloc((size_t)globals.n + 1, sizeof(Bind *)), xcalloc((size_t)globals.n + 1, 1),
             xcalloc((size_t)all_lambdas.n + 1, 1)};
  for (int k = 0; k < nchunks; k++) {
    for (int i = 0; i < chunks[k].nraw; i++) r.bind[chunks[k].raw[i]->gidx] = chunks[k].raw[i];
    for (int i = 0; i < chunks[k].ng; i++)
      for (int j = 0; j < chunks[k].groups[i].n; j++) r.bind[chunks[k].groups[i].b[j]->gidx] = chunks[k].groups[i].b[j];
  }
  Chunk *user = &chunks[nchunks - 1];
  for (int i = 0; i < user->ng; i++)
    for (int j = 0; j < user->groups[i].n; j++)
      if (user->groups[i].b[j]->init) reach_global(&r, user->groups[i].b[j]->gidx);
  return r;
}

char *cgen_program(Chunk *chunks, int nchunks) {
  Reach r = reach_program(chunks, nchunks);
  SB out = {0};
  sb_puts(&out, fk_rt_src);
  sb_printf(&out, "\n/* ---- generated program ---- */\n\nstatic V G[%d];\nstatic size_t G_n = %d;\n\n",
            globals.n ? globals.n : 1, globals.n);
  for (int i = 0; i < all_lambdas.n; i++)
    if (r.lam[all_lambdas.a[i]->id]) sb_printf(&out, "static V fn_%d(V self, V *a);\n", all_lambdas.a[i]->id);
  sb_puts(&out, "\n");
  for (int i = 0; i < all_lambdas.n; i++)
    if (r.lam[all_lambdas.a[i]->id]) cg_lambda(&out, all_lambdas.a[i]);
  sb_puts(&out, "static void program(void *arg) {\n  static V *roots = G;\n  fk_add_roots(&roots, &G_n);\n");
  for (uint32_t i = FK_NBUILTIN_CONS; i < fk_ncons; i++) {
    sb_printf(&out, "  fk_register_con(\"%s\", %d);\n", fk_cons[i].name, fk_cons[i].arity);
    if (!fk_cons[i].labels) continue;
    sb_puts(&out, "  { static const char *const ls[] = {");
    for (int j = 0; j < fk_cons[i].arity; j++) sb_printf(&out, "%s\"%s\"", j ? ", " : "", fk_cons[i].labels[j]);
    sb_printf(&out, "}; fk_con_labels(%u, ls); }\n", i);
  }
  for (int i = 0; i < globals.n; i++)
    if (globals.a[i].prim >= 0 && r.global[i]) sb_printf(&out, "  G[%d] = fk_prim_value(%d);\n", i, globals.a[i].prim);
  /* Per chunk, as in interp_install: functions, dictionaries, values. */
  for (int k = 0; k < nchunks; k++) {
    Chunk *c = &chunks[k];
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < c->nraw; i++)
        if (r.global[c->raw[i]->gidx]) init_global(&out, c->raw[i], pass);
      for (int i = 0; i < c->ng; i++)
        for (int j = 0; j < c->groups[i].n; j++)
          if (r.global[c->groups[i].b[j]->gidx]) init_global(&out, c->groups[i].b[j], pass);
    }
  }
  sb_puts(&out, "}\n\nint main(void) { return fk_run(program, 0); }\n");
  return out.s;
}
