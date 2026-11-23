/* pipeline.c -- a chunk's passes, in order.

   check (check.c): declarations registered, types inferred and defaulted.
   elab (elab.c): dictionary passing and type-class specialization.
   lift (lift.c): local functions only called become top-level functions.
   fuse (fuse.c): chains of list functions become one loop.
   ho (ho.c): specialization on function arguments.
   reuse (reuse.c): copies that write output cells into a consumed input's.
   resolve (resolve.c): variables to slots, and closures' free variables.

   For the compiler's developers, read from the environment:
   FK_SKIP=lift,fuse,ho,reuse  turns those passes off; the others are needed.
   FK_DUMP=pass,...            prints the chunk's bindings after each pass
                               named, but not the prelude's.
   FK_CHECK_TYPES=1            after each pass from elab on, reports an
                               expression left without a type (check.c). */
#include "funk.h"

static const char *const optional[] = {"lift", "fuse", "ho", "reuse"};
static const char *const all_passes[] = {"check", "elab", "lift", "fuse", "ho", "reuse", "resolve"};

/* Is pass named in the comma-separated list of variable var? With pass
   NULL, reports the names that are not in known. */
static bool listed(const char *var, const char *pass, const char *const *known, int nknown) {
  const char *s = getenv(var);
  if (!s) return false;
  bool found = false;
  for (const char *p = s; *p;) {
    size_t w = strcspn(p, ",");
    if (pass) {
      found |= w == strlen(pass) && !strncmp(p, pass, w);
    } else {
      bool ok = false;
      for (int i = 0; i < nknown; i++) ok |= w == strlen(known[i]) && !strncmp(p, known[i], w);
      if (!ok) fk_message("funk: ", "%s: no pass `%.*s`", var, (int)w, p);
    }
    p += w + (p[w] == ',');
  }
  return found;
}

static bool skipped(const char *pass) { return listed("FK_SKIP", pass, NULL, 0); }

/* ---------------------------------------------------------------- dump */

static void dump_expr(SB *b, Expr *e, int ind);

static void nl(SB *b, int ind) { sb_printf(b, "\n%*s", ind, ""); }

static void dump_pat(SB *b, Pat *p) {
  switch (p->k) {
  case P_VAR: sb_puts(b, p->name); return;
  case P_WILD: sb_puts(b, "_"); return;
  case P_INT: sb_printf(b, "%lld", p->ival); return;
  case P_CHAR: sb_printf(b, "'\\%lld'", p->ival); return;
  case P_STR: sb_printf(b, "\"%.*s\"", (int)p->ival, p->name); return;
  case P_CON:
    if (!p->n) {
      sb_puts(b, p->name);
      return;
    }
    sb_printf(b, "(%s", p->name);
    for (int i = 0; i < p->n; i++) {
      sb_puts(b, " ");
      dump_pat(b, p->args[i]);
    }
    sb_puts(b, ")");
    return;
  }
}

static void dump_group(SB *b, Group *g, int ind) {
  for (int i = 0; i < g->n; i++) {
    nl(b, ind);
    sb_printf(b, "%s = ", g->b[i]->name);
    dump_expr(b, g->b[i]->e, ind + 2);
  }
}

static void dump_rhs(SB *b, Rhs *r, int ind) {
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) {
      sb_puts(b, "| ");
      dump_expr(b, r->guards[i], ind);
      sb_puts(b, " ");
    }
    sb_puts(b, "-> ");
    dump_expr(b, r->bodies[i], ind + 2);
  }
  if (r->nwg) {
    nl(b, ind);
    sb_puts(b, "where");
    for (int i = 0; i < r->nwg; i++) dump_group(b, &r->wg[i], ind + 2);
  }
}

static void dump_expr(SB *b, Expr *e, int ind) {
  switch (e->k) {
  case E_VAR: sb_printf(b, "%s%s", e->own ? "own " : "", e->name); return;
  case E_CON: sb_puts(b, e->name); return;
  case E_INT: sb_printf(b, "%lld", e->ival); return;
  case E_FLOAT: sb_printf(b, "%g", e->fval); return;
  case E_CHAR: sb_printf(b, "'\\%lld'", e->ival); return;
  case E_STR: sb_printf(b, "\"%.*s\"", e->slen, e->name); return;
  case E_APP:
    sb_puts(b, "(");
    dump_expr(b, e->fn, ind);
    for (int i = 0; i < e->n; i++) {
      sb_puts(b, " ");
      dump_expr(b, e->args[i], ind);
    }
    sb_puts(b, ")");
    return;
  case E_LAM:
    sb_puts(b, "(\\");
    for (int i = 0; i < e->lam->n; i++) sb_printf(b, "%s ", e->lam->params[i]);
    sb_puts(b, "->");
    nl(b, ind);
    dump_expr(b, e->lam->body, ind);
    sb_puts(b, ")");
    return;
  case E_LET:
    sb_puts(b, "let");
    dump_group(b, &e->g, ind + 2);
    nl(b, ind);
    sb_puts(b, "in ");
    dump_expr(b, e->body, ind);
    return;
  case E_IF:
    sb_puts(b, "if ");
    dump_expr(b, e->cond, ind);
    nl(b, ind);
    sb_puts(b, "then ");
    dump_expr(b, e->then_, ind + 2);
    nl(b, ind);
    sb_puts(b, "else ");
    dump_expr(b, e->else_, ind + 2);
    return;
  case E_CASE:
    sb_puts(b, "case");
    for (int i = 0; i < e->ns; i++) {
      sb_puts(b, " ");
      dump_expr(b, e->scruts[i], ind);
    }
    sb_puts(b, " of");
    for (int i = 0; i < e->na; i++) {
      Alt *a = e->alts[i];
      nl(b, ind + 2);
      for (int j = 0; j < a->np; j++) {
        dump_pat(b, a->pats[j]);
        sb_puts(b, " ");
      }
      dump_rhs(b, &a->rhs, ind + 4);
    }
    return;
  case E_ANN: dump_expr(b, e->body, ind); return;
  default: sb_printf(b, "<expression kind %d>", e->k); return;
  }
}

static void dump(const char *pass, Bind **bs, int n, Bind **more, int nmore) {
  if (!strcmp(CS->cur_file, "<prelude>") || !listed("FK_DUMP", pass, NULL, 0)) return;
  SB b = {0};
  sb_printf(&b, "-- %s: after %s\n", CS->cur_file, pass);
  for (int k = 0; k < 2; k++)
    for (int i = 0; i < (k ? nmore : n); i++) {
      Bind *x = k ? more[i] : bs[i];
      sb_printf(&b, "%s = ", x->name);
      dump_expr(&b, x->e, 2);
      sb_puts(&b, "\n");
    }
  fk_write(2, b.s, strlen(b.s));
  xfree(b.s);
}

/* After pass: the chunk's bindings dumped and checked. */
static void after(const char *pass, Module *m) {
  dump(pass, m->binds, m->nb, CS->specialized.a, CS->specialized.n);
  if (strcmp(pass, "check")) {
    check_types(m->binds, m->nb, pass);
    check_types(CS->specialized.a, CS->specialized.n, pass);
  }
}

/* ---------------------------------------------------------------- passes */

bool compile_chunk(Module *m, Chunk *out) {
  jmp_buf jb, *saved = CS->cerr_jmp;
  ChunkMark mark = chunk_mark();
  listed("FK_SKIP", NULL, optional, 4);
  listed("FK_DUMP", NULL, all_passes, 7);
  if (setjmp(jb)) {
    compile_budget(false);
    elab_abort();
    CS->cerr_jmp = saved;
    chunk_undo(&mark);
    return false;
  }
  CS->cerr_jmp = &jb;
  compile_budget(true);
  Binds raw = {0};
  int first_inst, ng;
  Group *gs = check_chunk(m, &raw, &first_inst, &ng);
  after("check", m);

  CS->specialized.n = 0;
  for (int i = 0; i < m->nb; i++) elab_bind(m->binds[i]);
  if (CS->interactive)
    for (int i = 0; i < m->nb; i++) elab_default_value(m->binds[i]);
  after("elab", m);
  /* Clones of values, in the order made: an earlier input's run first; this
     input's, at their default types, after the input's own groups. */
  VEC(Group) vclones = {0}, vlate = {0};
  for (int i = 0; i < CS->specialized.n; i++) {
    Bind *b = CS->specialized.a[i];
    if (b->e->k == E_LAM) VPUSH(raw, b);
    else if (b->from->gidx < mark.globals) VPUSH(vclones, ((Group){&CS->specialized.a[i], 1, false}));
    else VPUSH(vlate, ((Group){&CS->specialized.a[i], 1, false}));
  }
  int nspec = CS->specialized.n;

  bool lift = !skipped("lift");
  if (lift) {
    lift_locals(m->binds, m->nb);
    lift_locals(CS->specialized.a, nspec); /* type-class clones made above */
    after("lift", m);
  }
  if (!skipped("fuse")) {
    Binds cand = {0};
    for (int i = 0; i < m->nb; i++) VPUSH(cand, m->binds[i]);
    for (int i = 0; i < CS->specialized.n; i++) VPUSH(cand, CS->specialized.a[i]);
    int nfused = fuse_lists(cand.a, cand.n);
    if (lift) lift_locals(cand.a, nfused); /* the fused loops */
    after("fuse", m);
  }
  if (!skipped("ho")) {
    ho_begin();
    ho_specialize(m->binds, m->nb);
    ho_specialize(CS->specialized.a, CS->specialized.n); /* clones and lifted functions */
    after("ho", m);
  }
  if (!skipped("reuse")) {
    reuse_cells(m->binds, m->nb, CS->specialized.a, CS->specialized.n);
    after("reuse", m);
  }

  for (int i = nspec; i < CS->specialized.n; i++) VPUSH(raw, CS->specialized.a[i]);
  /* Dictionaries after the selectors, superclass dictionaries first. */
  for (int depth = 1; depth <= 64; depth++)
    for (int i = first_inst; i < CS->insts.n; i++)
      if (__builtin_popcountll(CS->classes.a[CS->insts.a[i].cls].closure) == depth) VPUSH(raw, dict_bind(&CS->insts.a[i]));
  for (int i = 0; i < m->nb; i++) resolve_top(m->binds[i]);
  if (vclones.n || vlate.n) {
    for (int i = 0; i < vclones.n; i++) resolve_top(vclones.a[i].b[0]);
    for (int i = 0; i < vlate.n; i++) resolve_top(vlate.a[i].b[0]);
    Group *all = xmalloc((size_t)(vclones.n + ng + vlate.n) * sizeof *all);
    copy_n(all, vclones.a, (size_t)vclones.n * sizeof *all);
    copy_n(all + vclones.n, gs, (size_t)ng * sizeof *all);
    copy_n(all + vclones.n + ng, vlate.a, (size_t)vlate.n * sizeof *all);
    gs = all;
    ng += vclones.n + vlate.n;
  }
  for (int i = 0; i < raw.n; i++) {
    Bind *b = raw.a[i];
    b->gidx = global_lookup(b->name);
    if (b->e->k == E_LAM) CS->globals.a[b->gidx].lam = b->e->lam;
    resolve_top(b);
  }
  dump("resolve", m->binds, m->nb, raw.a, raw.n);
  check_late(m);

  out->groups = gs;
  out->ng = ng;
  out->raw = raw.a;
  out->nraw = raw.n;
  for (int i = 0; i < m->nlibs; i++) {
    bool seen = false;
    for (int j = 0; j < CS->ffi_libs.n; j++) seen |= !strcmp(CS->ffi_libs.a[j], m->libs[i]);
    if (!seen) VPUSH(CS->ffi_libs, m->libs[i]);
  }
  compile_budget(false);
  CS->cerr_jmp = saved;
  return true;
}
