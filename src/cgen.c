/* cgen.c -- emit a standalone C program: the runtime source, then one C
   function per lambda, then an initializer for the globals.

   Frame slots become C locals l<i>, free variables are read from the
   closure. Saturated calls to known lambdas, primitives and constructors
   are direct; anything else goes through fk_apply. A self call in tail
   position becomes a jump back to the top of the function. Arguments are
   evaluated left to right into temporaries before each call, since C
   leaves argument order unspecified. */
#include "funk.h"

const char *prim_cname(int i);

typedef struct {
  SB *out;
  Lambda *lam;
  int tmp, label;
  bool looped;
} CG;

static const char *cg_expr(CG *c, Expr *e);
static void cg_tail(CG *c, Expr *e);

static const char *newtmp(CG *c) { return xsprintf("t%d", c->tmp++); }

static const char *var_ref(Expr *e) {
  switch (e->vk) {
  case VK_LOCAL: return xsprintf("l%d", e->idx);
  case VK_FREE: return xsprintf("FK_FREE(self, %d)", e->idx);
  default: return xsprintf("G[%d]", e->idx);
  }
}

static const char *src_ref(FreeSrc s) {
  return s.vk == VK_LOCAL ? xsprintf("l%d", s.idx) : xsprintf("FK_FREE(self, %d)", s.idx);
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
  for (int i = 0; i < L->nfree; i++) sb_printf(c->out, "  FK_FREE(%s, %d) = %s;\n", v, i, src_ref(L->free[i]));
}

static void cg_group(CG *c, Group *g) {
  if (!g->rec) {
    Lambda *L = g->b[0]->thunk;
    const char *v;
    if (L) {
      v = newtmp(c);
      sb_printf(c->out, "  V %s = fk_thunk(fn_%d, %d, 0);\n", v, L->id, L->nfree);
      closure_fill(c, v, L);
    } else {
      v = cg_expr(c, g->b[0]->e);
    }
    sb_printf(c->out, "  l%d = %s;\n", g->b[0]->slot, v);
    return;
  }
  for (int i = 0; i < g->n; i++)
    sb_printf(c->out, "  l%d = %s;\n", g->b[i]->slot, closure_new(c, g->b[i]->e->lam));
  for (int i = 0; i < g->n; i++) closure_fill(c, xsprintf("l%d", g->b[i]->slot), g->b[i]->e->lam);
}

/* Primitives with an inline form fk_i_NAME(args) in rt.c. */
static bool inline_prim(const char *c) {
  static const char *const names[] = {"addi", "subi", "muli", "addf", "subf", "mulf", "negi", "negf", "absi",
                                      "absf", "fdiv", "eqw",  "new",  "ltw",  "lew",  "gtw",  "gew",  "eqf",
                                      "nef",  "ltf",  "lef",  "gtf",  "gef",  "div",  "mod",  "quot", "rem"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    if (strcmp(names[i], c) == 0) return true;
  return false;
}

static bool compare_prim(const char *c) {
  static const char *const names[] = {"eqw", "new", "ltw", "lew", "gtw", "gew",
                                      "eqf", "nef", "ltf", "lef", "gtf", "gef"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    if (strcmp(names[i], c) == 0) return true;
  return false;
}

/* A condition as a C truth value: an inline comparison directly, anything
   else compared with True. */
static const char *cg_cond(CG *c, Expr *e) {
  if (e->k == E_APP && e->n == 2 && e->fn->k == E_VAR && e->fn->vk == VK_GLOBAL) {
    int prim = globals.a[e->fn->idx].prim;
    if (prim >= 0 && compare_prim(prim_cname(prim))) {
      const char *x = cg_expr(c, e->args[0]), *y = cg_expr(c, e->args[1]);
      return xsprintf("fk_b_%s(%s, %s)", prim_cname(prim), x, y);
    }
  }
  return xsprintf("%s == fk_true", cg_expr(c, e));
}

/* The call for an application, as a C expression; args already evaluated. */
static const char *cg_call(CG *c, Expr *e) {
  Expr *f = e->fn;
  int n = e->n;
  const char *fv = NULL;
  int g = f->k == E_VAR && f->vk == VK_GLOBAL ? f->idx : -1;
  int prim = g >= 0 ? globals.a[g].prim : -1;
  bool direct = f->k == E_CON || prim >= 0 || (f->k == E_VAR && f->known);
  if (!direct) fv = cg_expr(c, f);
  const char **a = xmalloc((size_t)n * sizeof *a);
  for (int i = 0; i < n; i++) a[i] = cg_expr(c, e->args[i]);
  int arity;
  const char *call;
  if (f->k == E_CON) {
    arity = fk_cons[f->idx].arity;
    if (n < arity) return xsprintf("fk_apply(fk_con_value(%d), %d, %s)", f->idx, n, arg_array(a, n));
    call = xsprintf("fk_mkcon(%d, %d, %s)", f->idx, n, arg_array(a, n));
  } else if (prim >= 0) {
    arity = fk_prims[prim].arity;
    if (n < arity) return xsprintf("fk_apply(G[%d], %d, %s)", g, n, arg_array(a, n));
    if (inline_prim(prim_cname(prim)))
      call = arity == 1 ? xsprintf("fk_i_%s(%s)", prim_cname(prim), a[0])
                        : xsprintf("fk_i_%s(%s, %s)", prim_cname(prim), a[0], a[1]);
    else
      call = xsprintf("fk_p_%s(0, %s)", prim_cname(prim), arg_array(a, arity));
  } else if (direct) {
    arity = f->known->n;
    if (n < arity) return xsprintf("fk_apply(%s, %d, %s)", var_ref(f), n, arg_array(a, n));
    call = xsprintf("fn_%d(%s, %s)", f->known->id, var_ref(f), arg_array(a, arity));
  } else {
    return xsprintf("fk_apply(%s, %d, %s)", fv, n, arg_array(a, n));
  }
  if (n > arity) call = xsprintf("fk_apply(%s, %d, %s)", call, n - arity, arg_array(a + arity, n - arity));
  return call;
}

static void pat_cond(SB *cond, Pat *p, const char *path) {
  switch (p->k) {
  case P_VAR:
  case P_WILD: return;
  case P_INT: sb_printf(cond, " && fk_num_eq_int(%s, %lldLL)", path, p->ival); return;
  case P_CHAR: sb_printf(cond, " && %s == FK_MKCHAR(%lld)", path, p->ival); return;
  default:
    if (tydefs.a[condefs.a[p->con].tydef].ncons > 1) sb_printf(cond, " && FK_CONID(%s) == %d", path, p->con);
    for (int i = 0; i < p->n; i++) pat_cond(cond, p->args[i], xsprintf("FK_FIELD(%s, %d)", path, i));
  }
}

static void pat_bind(SB *out, Pat *p, const char *path) {
  if (p->k == P_VAR) sb_printf(out, "  l%d = %s;\n", p->slot, path);
  for (int i = 0; p->k == P_CON && i < p->n; i++) pat_bind(out, p->args[i], xsprintf("FK_FIELD(%s, %d)", path, i));
}

/* Alternatives are tried in order; a failed pattern or a fallthrough
   past every guard jumps to the next one. */
static const char *cg_case(CG *c, Expr *e, bool tail) {
  const char **sv = xmalloc((size_t)e->ns * sizeof *sv + 1);
  for (int i = 0; i < e->ns; i++) {
    sv[i] = newtmp(c);
    sb_printf(c->out, "  V %s = %s;\n", sv[i], cg_expr(c, e->scruts[i]));
  }
  const char *r = NULL;
  int done = c->label++;
  if (!tail) {
    r = newtmp(c);
    sb_printf(c->out, "  V %s = 0;\n", r);
  }
  for (int a = 0; a < e->na; a++) {
    Alt *alt = e->alts[a];
    int next = c->label++;
    SB cond = {0};
    sb_puts(&cond, "");
    for (int i = 0; i < alt->np; i++) pat_cond(&cond, alt->pats[i], sv[i]);
    if (cond.n) sb_printf(c->out, "  if (!(1%s)) goto L%d;\n", cond.s, next);
    sb_puts(c->out, "  {\n");
    for (int i = 0; i < alt->np; i++) pat_bind(c->out, alt->pats[i], sv[i]);
    Rhs *rh = &alt->rhs;
    for (int i = 0; i < rh->nwg; i++) cg_group(c, &rh->wg[i]);
    bool total = false;
    for (int i = 0; i < rh->ng && !total; i++) {
      if (rh->guards[i]) {
        sb_printf(c->out, "  if (%s) {\n", cg_cond(c, rh->guards[i]));
      } else {
        total = true;
        sb_puts(c->out, "  {\n");
      }
      if (tail) {
        cg_tail(c, rh->bodies[i]);
      } else {
        const char *v = cg_expr(c, rh->bodies[i]);
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
  return r;
}

static const char *cg_expr(CG *c, Expr *e) {
  const char *t;
  switch (e->k) {
  case E_VAR:
    if (!e->lazy) return var_ref(e);
    t = newtmp(c); /* a temporary keeps forcing in evaluation order */
    sb_printf(c->out, "  V %s = fk_force(%s);\n", t, var_ref(e));
    return t;
  case E_CON: return xsprintf("fk_con_value(%d)", e->idx);
  case E_INT: return xsprintf("FK_MKINT(%lldLL)", e->ival);
  case E_CHAR: return xsprintf("FK_MKCHAR(%lld)", e->ival);
  case E_FLOAT:
    t = newtmp(c);
    sb_printf(c->out, "  V %s = fk_float(%.17g);\n", t, e->fval);
    return t;
  case E_STR: {
    SB s = {0};
    c_string(&s, e->name, e->slen);
    t = newtmp(c);
    sb_printf(c->out, "  V %s = fk_string(%s, %d);\n", t, s.s, e->slen);
    return t;
  }
  case E_APP: {
    const char *call = cg_call(c, e);
    t = newtmp(c);
    sb_printf(c->out, "  V %s = %s;\n", t, call);
    return t;
  }
  case E_LAM:
    t = closure_new(c, e->lam);
    closure_fill(c, t, e->lam);
    return t;
  case E_LET: cg_group(c, &e->g); return cg_expr(c, e->body);
  case E_IF: {
    const char *cond = cg_cond(c, e->cond);
    t = newtmp(c);
    sb_printf(c->out, "  V %s;\n  if (%s) {\n", t, cond);
    sb_printf(c->out, "  %s = %s;\n  } else {\n", t, cg_expr(c, e->then_));
    sb_printf(c->out, "  %s = %s;\n  }\n", t, cg_expr(c, e->else_));
    return t;
  }
  case E_CASE: return cg_case(c, e, false);
  case E_ANN: return cg_expr(c, e->body);
  }
  abort();
}

static void cg_tail(CG *c, Expr *e) {
  switch (e->k) {
  case E_APP: {
    Expr *f = e->fn;
    if (f->k == E_VAR && f->known == c->lam && c->lam->n == e->n) {
      const char **a = xmalloc((size_t)e->n * sizeof *a);
      for (int i = 0; i < e->n; i++) a[i] = cg_expr(c, e->args[i]);
      int base = c->tmp;
      c->tmp += e->n;
      for (int i = 0; i < e->n; i++) sb_printf(c->out, "  V t%d = %s;\n", base + i, a[i]);
      for (int i = 0; i < e->n; i++) sb_printf(c->out, "  l%d = t%d;\n", i, base + i);
      sb_puts(c->out, "  goto top;\n");
      c->looped = true;
      return;
    }
    sb_printf(c->out, "  return %s;\n", cg_call(c, e));
    return;
  }
  case E_LET: cg_group(c, &e->g); cg_tail(c, e->body); return;
  case E_IF: {
    sb_printf(c->out, "  if (%s) {\n", cg_cond(c, e->cond));
    cg_tail(c, e->then_);
    sb_puts(c->out, "  } else {\n");
    cg_tail(c, e->else_);
    sb_puts(c->out, "  }\n");
    return;
  }
  case E_CASE: cg_case(c, e, true); return;
  case E_ANN: cg_tail(c, e->body); return;
  default: sb_printf(c->out, "  return %s;\n", cg_expr(c, e)); return;
  }
}

static void cg_lambda(SB *out, Lambda *L) {
  SB body = {0};
  sb_puts(&body, "");
  CG c = {&body, L, 0, 0, false};
  cg_tail(&c, L->body);
  char *nm = xsprintf("%s", L->name ? L->name : "lambda");
  for (char *s = nm; (s = strstr(s, "*/"));) s[1] = '|'; /* keep the comment closed */
  sb_printf(out, "static V fn_%d(V self, V *a) { /* %s */\n", L->id, nm);
  for (int i = 0; i < L->nslots; i++) sb_printf(out, "  V l%d = %s;\n", i, i < L->n ? xsprintf("a[%d]", i) : "0");
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
