/* cgen.c -- emit a standalone C program: the runtime source, then one C
   function per lambda, then an initializer for the globals.

   Frame slots become C locals l<i>, free variables are read from the
   closure. Saturated calls to known lambdas, primitives and constructors
   are direct; anything else goes through fk_apply. Values of scalar type
   are C scalars inside a function body; function boundaries, data fields
   and closures use V (typed-backend.md). Each lambda is a worker w_N
   with C-typed parameters and result, called directly by saturated known
   calls, and a wrapper fn_N(self, V *a) for closures, thunks and
   fk_apply. A self call in tail
   position becomes a jump back to the top of the function. Arguments are
   evaluated left to right into temporaries before each call, since C
   leaves argument order unspecified. */
#include <math.h>

#include "funk.h"

const char *prim_cname(int i);

/* A C representation. Monomorphic values of scalar type live in C scalars
   (typed-backend.md); everything else is a tagged word V. */
typedef enum { R_V, R_I, R_F, R_C, R_B, R_P } Rep; /* R_P: a Ptr, FunPtr or StablePtr as a C address */
typedef struct { const char *s; Rep r; } TV; /* a C expression and its representation */

static const char *const ctype[] = {"V", "int64_t", "double", "int32_t", "bool", "void *"};

typedef struct {
  SB *out;
  Lambda *lam;
  int tmp, label;
  bool looped;
  Rep *slot; /* representation of each frame slot */
  bool trmc; /* results go through dst; see cg_tail */
  const char *reuse; /* a dead cons cell, matched from an `own` scrutinee */
  Rep rrep;          /* the worker's result */
  bool reused;       /* the body reuses a cell somewhere */
} CG;

static TV cg_expr(CG *c, Expr *e);
static void cg_tail(CG *c, Expr *e);

static Rep rep_of(Type *t) {
  if (!t) return R_V;
  switch (ffi_kind(t)) { /* the C types of foreign imports are held as Int or Float; Ptr a as an address */
  case 'i': return R_I;
  case 'f': return R_F;
  case 'c': return R_C;
  case 'b': return R_B;
  case 'p': return R_P;
  default: return R_V;
  }
}

/* v converted to representation to; boxing a Float allocates. */
static const char *conv(TV v, Rep to) {
  if (v.r == to) return v.s;
  if (v.r != R_V && to != R_V) return conv((TV){conv(v, R_V), R_V}, to);
  if (to == R_V) {
    static const char *const box[] = {NULL, "FK_MKINT(%s)", "fk_float(%s)", "FK_MKCHAR(%s)", "FK_BOOL(%s)", "fk_ptr(%s)"};
    return xsprintf(box[v.r], v.s);
  }
  static const char *const unbox[] = {NULL, "FK_INT(%s)", "FK_FLOATV(%s)", "FK_CHAR(%s)", "FK_ISTRUE(%s)", "FK_PTRV(%s)"};
  return xsprintf(unbox[to], v.s);
}

static TV tv(const char *s, Rep r) { return (TV){s, r}; }

/* A worker's parameter and result representations. */
static Rep param_rep(Lambda *L, int i) { return L->ptypes ? rep_of(L->ptypes[i]) : R_V; }
static Rep result_rep(Lambda *L) { return rep_of(L->body->ty); }
static const char *as_v(TV v) { return conv(v, R_V); }

static const char *newtmp(CG *c) { return xsprintf("t%d", c->tmp++); }

/* A new temporary holding v, so its evaluation happens here. */
static TV hold(CG *c, TV v) {
  const char *t = newtmp(c);
  sb_printf(c->out, "  %s %s = %s;\n", ctype[v.r], t, v.s);
  return tv(t, v.r);
}

/* Representations of each lambda's free variables, by lambda id. A
   scalar is stored raw in its closure slot, so capturing a Float does not
   allocate. Set before generation, from the slots of the function that
   creates the closure; stores and reads both use this table. */
static Rep **freps;

static Rep frep(Lambda *L, int i) { return freps[L->id] ? freps[L->id][i] : R_V; }

static TV raw_read(const char *w, Rep r) {
  static const char *const fmt[] = {"%s", "((int64_t)%s)", "fk_rawf(%s)", "((int32_t)%s)", "((bool)%s)", "((void *)%s)"};
  return tv(xsprintf(fmt[r], w), r);
}

static const char *raw_word(TV v, Rep r) {
  const char *s = conv(v, r);
  if (r == R_V) return s;
  return r == R_F ? xsprintf("fk_fraw(%s)", s) : r == R_P ? xsprintf("((V)(uintptr_t)%s)", s) : xsprintf("((V)(int64_t)%s)", s);
}

/* Fields declared `Float` hold a raw double (typed-backend.md, step 5).
   Bit i of the mask: field i of constructor con. The runtime learns the
   masks from fk_con_raw calls at startup. */
static uint32_t con_raw(int con) {
  Type *t = condefs.a[con].scheme;
  uint32_t m = 0;
  for (int i = 0; t && i < 32; i++) {
    t = repr(t);
    if (t->k != TY_CON || strcmp(t->name, "->") != 0) break;
    if (rep_of(t->args[0]) == R_F) m |= 1u << i;
    t = t->args[1];
  }
  return m;
}

/* Field i of con at path, with its representation; and a value as the
   word to store there. */
static TV field_read(int con, int i, const char *path) {
  const char *w = xsprintf("FK_FIELD(%s, %d)", path, i);
  return i < 32 && (con_raw(con) >> i & 1) ? tv(xsprintf("fk_rawf(%s)", w), R_F) : tv(w, R_V);
}

static const char *field_word(int con, int i, TV v) {
  return i < 32 && (con_raw(con) >> i & 1) ? xsprintf("fk_fraw(%s)", conv(v, R_F)) : as_v(v);
}

static TV var_ref(CG *c, Expr *e) {
  switch (e->vk) {
  case VK_LOCAL: return tv(xsprintf("l%d", e->idx), c->slot[e->idx]);
  case VK_FREE: return raw_read(xsprintf("FK_FREE(self, %d)", e->idx), frep(c->lam, e->idx));
  default: return tv(xsprintf("G[%d]", e->idx), R_V);
  }
}

static TV src_ref(CG *c, FreeSrc s) {
  if (s.vk == VK_LOCAL) return tv(xsprintf("l%d", s.idx), c->slot[s.idx]);
  return raw_read(xsprintf("FK_FREE(self, %d)", s.idx), frep(c->lam, s.idx));
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
  for (int i = 0; i < L->nfree; i++)
    sb_printf(c->out, "  FK_FREE(%s, %d) = %s;\n", v, i, raw_word(src_ref(c, L->free[i]), frep(L, i)));
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
typedef struct { const char *name, *fmt; int nargs; int arg; Rep res; const char *reps; } Scalar; /* reps: each argument's, when they differ: "ipf" */
static const Scalar scalars[] = {
    {"addi", "(int64_t)((uint64_t)%s + (uint64_t)%s)", 2, R_I, R_I, NULL},
    {"subi", "(int64_t)((uint64_t)%s - (uint64_t)%s)", 2, R_I, R_I, NULL},
    {"muli", "(int64_t)((uint64_t)%s * (uint64_t)%s)", 2, R_I, R_I, NULL},
    {"negi", "(int64_t)(0 - (uint64_t)%s)", 1, R_I, R_I, NULL},
    {"absi", "fk_s_absi(%s)", 1, R_I, R_I, NULL},
    {"div", "fk_s_div(%s, %s)", 2, R_I, R_I, NULL},
    {"mod", "fk_s_mod(%s, %s)", 2, R_I, R_I, NULL},
    {"quot", "fk_s_quot(%s, %s)", 2, R_I, R_I, NULL},
    {"rem", "fk_s_rem(%s, %s)", 2, R_I, R_I, NULL},
    {"addf", "(%s + %s)", 2, R_F, R_F, NULL},
    {"subf", "(%s - %s)", 2, R_F, R_F, NULL},
    {"mulf", "(%s * %s)", 2, R_F, R_F, NULL},
    {"fdiv", "(%s / %s)", 2, R_F, R_F, NULL},
    {"negf", "(-%s)", 1, R_F, R_F, NULL},
    {"absf", "fabs(%s)", 1, R_F, R_F, NULL},
    {"sqrt", "sqrt(%s)", 1, R_F, R_F, NULL},
    {"exp", "exp(%s)", 1, R_F, R_F, NULL},
    {"log", "log(%s)", 1, R_F, R_F, NULL},
    {"sin", "sin(%s)", 1, R_F, R_F, NULL},
    {"cos", "cos(%s)", 1, R_F, R_F, NULL},
    {"pow", "pow(%s, %s)", 2, R_F, R_F, NULL},
    {"tofloat", "((double)%s)", 1, R_I, R_F, NULL},
    {"ord", "((int64_t)%s)", 1, R_C, R_I, NULL},
    {"chr", "fk_s_chr(%s)", 1, R_I, R_C, NULL},
    {"eqw", "(%s == %s)", 2, -1, R_B, NULL},
    {"new", "(%s != %s)", 2, -1, R_B, NULL},
    {"ltw", "(%s < %s)", 2, -1, R_B, NULL},
    {"lew", "(%s <= %s)", 2, -1, R_B, NULL},
    {"gtw", "(%s > %s)", 2, -1, R_B, NULL},
    {"gew", "(%s >= %s)", 2, -1, R_B, NULL},
    {"eqf", "(%s == %s)", 2, R_F, R_B, NULL},
    {"nef", "(%s != %s)", 2, R_F, R_B, NULL},
    {"ltf", "(%s < %s)", 2, R_F, R_B, NULL},
    {"lef", "(%s <= %s)", 2, R_F, R_B, NULL},
    {"gtf", "(%s > %s)", 2, R_F, R_B, NULL},
    {"gef", "(%s >= %s)", 2, R_F, R_B, NULL},
    {"idi", "%s", 1, R_I, R_I, NULL},
    {"idf", "%s", 1, R_F, R_F, NULL},
    {"tocint", "(int64_t)(int32_t)(uint32_t)%s", 1, R_I, R_I, NULL},
    {"tocuint", "(int64_t)(uint32_t)%s", 1, R_I, R_I, NULL},
    {"toclong", "(int64_t)(long)%s", 1, R_I, R_I, NULL},
    {"toculong", "(int64_t)(unsigned long)%s", 1, R_I, R_I, NULL},
    {"tocsize", "(int64_t)(size_t)%s", 1, R_I, R_I, NULL},
    {"toccchar", "(int64_t)(char)%s", 1, R_I, R_I, NULL},
    {"tocbool", "(int64_t)(%s != 0)", 1, R_I, R_I, NULL},
    /* Ptr operations and peek and poke, on C addresses (realtime.md, R4) */
    {"castptr", "%s", 1, R_P, R_P, NULL},
    {"eqptr", "(%s == %s)", 2, R_P, R_B, NULL},
    {"ptrint", "((int64_t)(uintptr_t)%s)", 1, R_P, R_I, NULL},
    {"plusptr", "((void *)((uintptr_t)%s + (uintptr_t)%s))", 2, -1, R_P, "pi"},
    {"nullptr", "((void)%s, (void *)0)", 1, R_V, R_P, NULL},
    {"peeki", "fk_s_peeki(%s, %s)", 2, -1, R_I, "ip"},
    {"peekf", "fk_s_peekf(%s, %s)", 2, -1, R_F, "ip"},
    {"peekp", "(*(void **)%s)", 1, R_P, R_P, NULL},
    {"pokei", "fk_s_pokei(%s, %s, %s)", 3, -1, R_V, "ipi"},
    {"pokef", "fk_s_pokef(%s, %s, %s)", 3, -1, R_V, "ipf"},
    {"pokep", "(*(void **)%s = %s, fk_unit)", 2, R_P, R_V, NULL},
    {"alen", "fk_s_alen(%s)", 1, R_P, R_I, NULL},
    {"asize", "fk_s_asize(%s)", 1, R_P, R_I, NULL},
    {"aptr", "fk_s_aptr(%s)", 1, R_P, R_P, NULL},
    {"wrap", "fk_s_wrap(%s, %s)", 2, R_I, R_I, NULL},
    {"truncsat", "fk_s_truncsat(%s)", 1, R_F, R_I, NULL},
    {"rcount", "fk_s_rcount(%s)", 1, R_P, R_I, NULL},
    {"rspace", "fk_s_rspace(%s)", 1, R_P, R_I, NULL},
    {"rback", "fk_s_rback(%s)", 1, R_P, R_P, NULL},
    {"rcommit", "fk_s_rcommit(%s)", 1, R_P, R_V, NULL},
    {"rfront", "fk_s_rfront(%s)", 1, R_P, R_P, NULL},
    {"rdrop", "fk_s_rdrop(%s)", 1, R_P, R_V, NULL},
    {"apeeki", "fk_s_apeeki(%s, %s)", 2, -1, R_I, "ip"},
    {"apeekf", "fk_s_apeekf(%s, %s)", 2, -1, R_F, "ip"},
    {"apokei", "fk_s_apokei(%s, %s, %s)", 3, -1, R_V, "ipi"},
    {"apokef", "fk_s_apokef(%s, %s, %s)", 3, -1, R_V, "ipf"},
    {"tocfloat", "(double)(float)%s", 1, R_F, R_F, NULL},
    {"ltu", "((uint64_t)%s < (uint64_t)%s)", 2, R_I, R_B, NULL},
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
  if (sc && sc->reps) {
    static const char codes[] = "vifcbp"; /* Rep order */
    const char *a[3];
    for (int i = 0; i < n; i++) a[i] = conv(cg_expr(c, e->args[i]), (Rep)(strchr(codes, sc->reps[i]) - codes));
    return tv(n == 2 ? xsprintf(sc->fmt, a[0], a[1]) : xsprintf(sc->fmt, a[0], a[1], a[2]), sc->res);
  }
  if (sc) {
    TV x[2];
    for (int i = 0; i < n; i++) x[i] = cg_expr(c, e->args[i]);
    Rep ar = sc->arg >= 0 ? (Rep)sc->arg : rep_of(e->args[0]->ty);
    if (ar == R_V) ar = rep_of(e->args[1 % n]->ty); /* an untyped first argument */
    const char *a0 = conv(x[0], ar), *a1 = n > 1 ? conv(x[1], ar) : NULL;
    if (ar == R_V && sc->arg < 0) /* tagged words: a boxed Int compares by value */
      return tv(xsprintf("fk_b_%s(%s, %s)", sc->name, a0, a1), R_B);
    return tv(n == 1 ? xsprintf(sc->fmt, a0) : xsprintf(sc->fmt, a0, a1), sc->res);
  }
  ForeignDecl *ffi = g >= 0 ? globals.a[g].ffi : NULL;
  bool scalar = ffi != NULL;
  if (ffi) {
    Type *t = repr(globals.a[g].type);
    for (int i = 0; i < ffi->arity; i++, t = repr(t->args[1])) scalar &= ffi_kind(t->args[0]) != 's';
    scalar &= ffi_kind(t) != 's';
  }
  if (ffi && n == ffi->arity && !scalar) { /* String and Ptr convert in the wrapper */
    const char **a = xmalloc((size_t)n * sizeof *a);
    for (int i = 0; i < n; i++) a[i] = as_v(cg_expr(c, e->args[i]));
    return tv(xsprintf("fk_ffi_%d(0, %s)", g, arg_array(a, n)), R_V);
  }
  if (ffi && n == ffi->arity) { /* the C function itself; the wrapper has checked the types */
    SB call = {0};
    sb_printf(&call, "%s(", ffi->cname);
    Type *t = repr(globals.a[g].type);
    for (int i = 0; i < ffi->arity; i++, t = repr(t->args[1])) {
      const char *x = conv(cg_expr(c, e->args[i]), rep_of(t->args[0]));
      if (ffi_kind(t->args[0]) != 'v') sb_printf(&call, "%s(%s)%s", i ? ", " : "", ffi_param_type(t->args[0]), x);
    }
    sb_puts(&call, ")");
    Rep r = rep_of(t); /* a scalar: the result is never applied further */
    TV res = hold(c, r == R_V   ? tv(xsprintf("((void)%s, fk_unit)", call.s), R_V)
                     : r == R_B ? tv(xsprintf("(%s != 0)", call.s), R_B)
                     : r == R_C ? tv(xsprintf("(int32_t)(unsigned char)%s", call.s), R_C)
                                : tv(xsprintf("(%s)%s", ctype[r], call.s), r));
    sb_puts(c->out, "  fk_cb_check();\n"); /* a direct callback that failed during the call */
    return res;
  }
  bool direct = f->k == E_CON || prim >= 0 || (f->k == E_VAR && f->known);
  const char *fv = direct ? NULL : as_v(cg_expr(c, f));
  Lambda *K = f->k == E_VAR && prim < 0 ? f->known : NULL;
  bool worker = K && n >= K->n;
  bool con = f->k == E_CON && n >= fk_cons[f->idx].arity; /* a partial application takes boxed fields */
  const char **a = xmalloc((size_t)n * sizeof *a);
  for (int i = 0; i < n; i++) {
    TV x = cg_expr(c, e->args[i]);
    a[i] = con ? field_word(f->idx, i, x) : conv(x, worker && i < K->n ? param_rep(K, i) : R_V);
  }
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
    SB w = {0};
    sb_printf(&w, "w_%d(%s", K->id, self);
    for (int i = 0; i < arity; i++) sb_printf(&w, ", %s", a[i]);
    sb_puts(&w, ")");
    if (n == arity) return tv(w.s, result_rep(K));
    call = as_v(tv(w.s, result_rep(K)));
  } else {
    return tv(xsprintf("fk_apply(%s, %d, %s)", fv, n, arg_array(a, n)), R_V);
  }
  if (n > arity) call = xsprintf("fk_apply(%s, %d, %s)", call, n - arity, arg_array(a + arity, n - arity));
  return tv(call, R_V);
}

/* ------------------------------------------- allocation-free (R4) */

/* Exported functions run on foreign threads, so their compiled code must
   not allocate, and must not fail, since a runtime error there ends the
   program (realtime.md, R4). The check walks the typed body after
   elaboration, and every function it calls. It accepts what compiles to
   C scalars and addresses: literals, variables of scalar types, unboxed
   primitives, C imports, calls to checked functions at full arity, if,
   case on scalars, and strict local values. */
static signed char *af_state; /* by lambda id: 0 unknown, 1 checking or allocation-free, 2 allocates */
static const char **af_why;
static int af_cap;

/* A type whose constructors all take no arguments, such as Ordering: its
   values are static objects. */
static bool enum_type(Type *t) {
  bool any = false;
  for (int c = 0; c < condefs.n; c++) {
    if (strcmp(tydefs.a[condefs.a[c].tydef].name, t->name) != 0) continue;
    if (condefs.a[c].arity) return false;
    any = true;
  }
  return any;
}

static bool af_type(Type *t) { /* held without a heap object */
  Rep r = rep_of(t);
  if (r == R_I || r == R_C || r == R_B) return true; /* a boxed Int beyond 63 bits only appears in V */
  if (r == R_F || r == R_P) return true;
  t = t ? repr(t) : NULL;
  return t && t->k == TY_CON && !t->n && enum_type(t); /* (), Ordering */
}

static const char *af_lambda(Lambda *L);
static const char *af_expr(Expr *e, Lambda *L);

static const char *af_at(const char *what, Expr *e, Lambda *L) {
  return xsprintf("%s, at %d:%d in `%s`", what, e->line, e->col, L->name ? L->name : "a local function");
}

static const char *af_group(Group *g, Lambda *L) {
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    if (g->rec || b->thunk) return af_at(xsprintf("`%s` is a lazy or recursive local binding", b->name), b->e, L);
    if (b->e->k == E_LAM) return af_at(xsprintf("`%s` is a local function, a closure", b->name), b->e, L);
    const char *w = af_expr(b->e, L);
    if (w) return w;
  }
  return NULL;
}

static bool af_pat(Pat *p) {
  return p->k == P_VAR || p->k == P_WILD || p->k == P_INT || p->k == P_CHAR || (p->k == P_CON && !p->n);
}

static const char *af_expr(Expr *e, Lambda *L) {
  const char *w;
  switch (e->k) {
  case E_INT: case E_FLOAT: case E_CHAR: return NULL;
  case E_STR: return af_at("a String", e, L);
  case E_LAM: return af_at("a closure", e, L);
  case E_CON:
    if (!fk_cons[e->idx].arity) return NULL; /* a static object */
    return af_at(xsprintf("the constructor `%s`", e->name), e, L);
  case E_VAR:
    if (e->lazy) return af_at(xsprintf("`%s` is lazy", e->name), e, L);
    if (!af_type(e->ty)) return af_at(xsprintf("`%s` has type `%s`, a heap value", e->name, type_str(e->ty)), e, L);
    return NULL;
  case E_ANN: return af_expr(e->body, L);
  case E_IF:
    if ((w = af_expr(e->cond, L)) || (w = af_expr(e->then_, L))) return w;
    return af_expr(e->else_, L);
  case E_LET:
    if ((w = af_group(&e->g, L))) return w;
    return af_expr(e->body, L);
  case E_CASE:
    if (!case_covers(e, true)) return af_at("a case that does not cover every value", e, L);
    for (int i = 0; i < e->ns; i++)
      if ((w = af_expr(e->scruts[i], L))) return w;
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      for (int i = 0; i < alt->np; i++)
        if (!af_pat(alt->pats[i])) return af_at("a pattern on a data type", e, L);
      for (int i = 0; i < alt->rhs.nwg; i++)
        if ((w = af_group(&alt->rhs.wg[i], L))) return w;
      for (int i = 0; i < alt->rhs.ng; i++)
        if ((alt->rhs.guards[i] && (w = af_expr(alt->rhs.guards[i], L))) || (w = af_expr(alt->rhs.bodies[i], L))) return w;
    }
    return NULL;
  case E_APP: {
    for (int i = 0; i < e->n; i++)
      if ((w = af_expr(e->args[i], L))) return w;
    Expr *f = e->fn;
    if (f->k == E_CON) return af_at(xsprintf("the constructor `%s`", f->name), e, L);
    if (f->k != E_VAR || f->vk != VK_GLOBAL) return af_at("a call of an unknown function", e, L);
    Global *g = &globals.a[f->idx];
    if (g->prim >= 0) {
      if (!prim_call_total(e)) return af_at(xsprintf("`%s` may fail; divide only by a non-zero literal", g->name), e, L);
      if (e->n == fk_prims[g->prim].arity && scalar_prim(prim_cname(g->prim))) return NULL;
      if (!strcmp(prim_cname(g->prim), "then") && e->n == 2) { /* its arguments are boxed: () and Bool box without allocating */
        for (int i = 0; i < 2; i++)
          if (rep_of(e->args[i]->ty) != R_V && rep_of(e->args[i]->ty) != R_B && rep_of(e->args[i]->ty) != R_C)
            return af_at(xsprintf("`>>` of a `%s`, which it boxes", type_str(e->args[i]->ty)), e, L);
        return NULL;
      }
      return af_at(xsprintf("the primitive `%s`", g->name), e, L);
    }
    if (g->ffi && !g->ffi->cb && e->n == g->ffi->arity) {
      bool str = false; /* converted in the wrapper, which allocates */
      Type *t = repr(g->type);
      for (int i = 0; i < g->ffi->arity; i++, t = repr(t->args[1])) str |= ffi_kind(t->args[0]) == 's';
      if (str || ffi_kind(t) == 's') return af_at(xsprintf("`%s` takes or returns a String", g->name), e, L);
      return NULL;
    }
    Lambda *K = f->known;
    if (!K || e->n != K->n) return af_at(xsprintf("`%s` is called with %d of %d arguments", g->name, e->n, K ? K->n : 0), e, L);
    return af_lambda(K);
  }
  default: return af_at("an expression the check does not handle", e, L);
  }
}

static const char *af_lambda(Lambda *L) {
  if (L->id >= af_cap) {
    int cap = L->id * 2 + 64;
    af_state = xrealloc(af_state, (size_t)cap);
    af_why = xrealloc(af_why, (size_t)cap * sizeof *af_why);
    memset(af_state + af_cap, 0, (size_t)(cap - af_cap));
    af_cap = cap;
  }
  if (af_state[L->id]) return af_state[L->id] == 2 ? af_why[L->id] : NULL;
  af_state[L->id] = 1; /* a recursive call is checked by the walk in progress */
  const char *w = NULL;
  for (int i = 0; i < L->n && !w; i++)
    if (!L->ptypes || !af_type(L->ptypes[i]))
      w = xsprintf("`%s` takes `%s`, a heap value", L->name ? L->name : "a function", L->ptypes ? type_str(L->ptypes[i]) : "an untyped argument");
  if (!w && !af_type(L->body->ty)) w = xsprintf("`%s` returns `%s`, a heap value", L->name ? L->name : "a function", type_str(L->body->ty));
  if (!w && L->nfree) w = xsprintf("`%s` is a closure", L->name ? L->name : "a function");
  if (!w) w = af_expr(L->body, L);
  if (w) {
    af_state[L->id] = 2;
    af_why[L->id] = w;
  }
  return w;
}

const char *cgen_alloc_free(Lambda *L) { return af_lambda(L); }

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
  case P_STR: {
    SB s = {0};
    c_string(&s, p->name, (int)p->ival);
    sb_printf(cond, " && fk_str_eq_lit(%s, %s, %lld)", path, s.s, p->ival);
    return;
  }
  default:
    if (r == R_B) {
      sb_printf(cond, p->con == FK_C_TRUE ? " && %s" : " && !%s", path);
      return;
    }
    if (tydefs.a[condefs.a[p->con].tydef].ncons > 1) sb_printf(cond, " && FK_CONID(%s) == %d", path, p->con);
    for (int i = 0; i < p->n; i++) {
      TV f = field_read(p->con, i, path);
      pat_cond(cond, p->args[i], f.s, f.r);
    }
  }
}

static void pat_bind(CG *c, Pat *p, const char *path, Rep r) {
  if (p->k == P_VAR) sb_printf(c->out, "  l%d = %s;\n", p->slot, conv(tv(path, r), c->slot[p->slot]));
  for (int i = 0; p->k == P_CON && r == R_V && i < p->n; i++) {
    TV f = field_read(p->con, i, path);
    pat_bind(c, p->args[i], f.s, f.r);
  }
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
  const char *reuse = c->reuse;
  for (int a = 0; a < e->na; a++) {
    Alt *alt = e->alts[a];
    int next = c->label++;
    c->reuse = reuse;
    for (int i = 0; i < alt->np; i++)
      if (e->scruts[i]->k == E_VAR && e->scruts[i]->own && alt->pats[i]->k == P_CON && alt->pats[i]->con == FK_C_CONS)
        c->reuse = sv[i].s, c->reused = true;
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
  c->reuse = reuse;
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
  case E_FLOAT: /* a hex float literal is exact, and keeps the sign of -0.0; %g printed it as the int -0 */
    if (isinf(e->fval)) return tv(e->fval > 0 ? "INFINITY" : "(-INFINITY)", R_F);
    return tv(xsprintf("%a", e->fval), R_F);
  case E_STR: { /* a static String, made on first use */
    static int nstr;
    SB s = {0};
    c_string(&s, e->name, e->slen);
    t = xsprintf("s%d", nstr++);
    sb_printf(c->out, "  static V %s;\n  if (!%s) %s = fk_str_static(%s, %d);\n", t, t, t, s.s, e->slen);
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

/* ------------------------------------------- tail recursion modulo cons */

/* A function whose tail result is a constructor with a call to itself as
   the last argument (`x : map f xs`) becomes a loop (memory.md). The
   result goes through `dst`, which points at `head` and then at the last
   field of the latest cell, and `head` is returned. The cell is allocated
   with a placeholder in that field, before the call's arguments are
   evaluated; the allocation is the only thing moved, so evaluation order
   is unchanged. */
static bool self_call(Lambda *L, Expr *e) {
  return e->k == E_APP && e->fn->k == E_VAR && e->fn->known == L && L->n == e->n;
}

static bool cons_self(Lambda *L, Expr *e) {
  return e->k == E_APP && e->fn->k == E_CON && e->n == fk_cons[e->fn->idx].arity && self_call(L, e->args[e->n - 1]);
}

static bool has_trmc(Lambda *L, Expr *e) {
  switch (e->k) {
  case E_APP: return cons_self(L, e);
  case E_LET: return has_trmc(L, e->body);
  case E_IF: return has_trmc(L, e->then_) || has_trmc(L, e->else_);
  case E_ANN: return has_trmc(L, e->body);
  case E_CASE:
    for (int a = 0; a < e->na; a++)
      for (int i = 0; i < e->alts[a]->rhs.ng; i++)
        if (has_trmc(L, e->alts[a]->rhs.bodies[i])) return true;
    return false;
  default: return false;
  }
}

/* *dst may be a reused cell's old tail, a heap pointer the write deletes
   (realtime.md, R2). In a body that reuses no cell it is the placeholder,
   and cg_lambda removes the barrier. */
#define SHADE_DST "  FK_SHADE(*dst);\n"

/* Return v from a function body, through dst in a TRMC loop. */
static void ret(CG *c, TV v) {
  if (c->trmc) sb_printf(c->out, SHADE_DST "  *dst = %s;\n  return head;\n", as_v(v));
  else sb_printf(c->out, "  return %s;\n", conv(v, c->rrep));
}

/* A self call in tail position: assign the parameters, jump to the top. */
static void self_jump(CG *c, Expr *e) {
  const char **a = xmalloc((size_t)e->n * sizeof *a);
  for (int i = 0; i < e->n; i++) a[i] = conv(cg_expr(c, e->args[i]), c->slot[i]);
  int base = c->tmp;
  c->tmp += e->n;
  for (int i = 0; i < e->n; i++) sb_printf(c->out, "  %s t%d = %s;\n", ctype[c->slot[i]], base + i, a[i]);
  for (int i = 0; i < e->n; i++) sb_printf(c->out, "  l%d = t%d;\n", i, base + i);
  sb_puts(c->out, "  goto top;\n");
  c->looped = true;
}

static void cg_tail(CG *c, Expr *e) {
  switch (e->k) {
  case E_APP: {
    if (self_call(c->lam, e)) {
      self_jump(c, e);
      return;
    }
    if (c->trmc && cons_self(c->lam, e)) {
      int n = e->n;
      const char **a = xmalloc((size_t)n * sizeof *a);
      for (int i = 0; i < n - 1; i++) a[i] = field_word(e->fn->idx, i, cg_expr(c, e->args[i]));
      a[n - 1] = "fk_unit"; /* the placeholder */
      const char *cell = newtmp(c);
      if (c->reuse && e->fn->idx == FK_C_CONS) /* the old tail stays until *dst overwrites it */
        sb_printf(c->out, "  V %s = %s;\n  FK_SHADE(FK_FIELD(%s, 0));\n  FK_FIELD(%s, 0) = %s;\n", cell, c->reuse, cell, cell, a[0]);
      else
        sb_printf(c->out, "  V %s = fk_mkcon(%d, %d, %s);\n", cell, e->fn->idx, n, arg_array(a, n));
      sb_printf(c->out, SHADE_DST "  *dst = %s;\n  dst = &FK_FIELD(%s, %d);\n", cell, cell, n - 1);
      self_jump(c, e->args[n - 1]);
      return;
    }
    ret(c, cg_call(c, e));
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
  default: ret(c, cg_expr(c, e)); return;
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

/* The wrapper's call of the worker, unboxing a. */
static const char *wrapper_call(Lambda *L) {
  SB b = {0};
  sb_printf(&b, "w_%d(self", L->id);
  for (int i = 0; i < L->n; i++) sb_printf(&b, ", %s", conv(tv(xsprintf("a[%d]", i), R_V), param_rep(L, i)));
  sb_puts(&b, ")");
  return b.s;
}

static Rep *lam_slots(Lambda *L) {
  Rep *slot = xcalloc((size_t)L->nslots + 1, sizeof *slot);
  for (int i = 0; i < L->n; i++) slot[i] = L->ptypes ? rep_of(L->ptypes[i]) : R_V;
  slots_expr(slot, L->body);
  return slot;
}

/* freps for the closures created in e, a part of P's body. */
static void freps_lambda(Lambda *L, Lambda *P, Rep *pslot);
static void freps_expr(Expr *e, Lambda *P, Rep *slot);

static void freps_group(Group *g, Lambda *P, Rep *slot) {
  for (int i = 0; i < g->n; i++) {
    if (g->b[i]->thunk) freps_lambda(g->b[i]->thunk, P, slot);
    else freps_expr(g->b[i]->e, P, slot);
  }
}

static void freps_expr(Expr *e, Lambda *P, Rep *slot) {
  switch (e->k) {
  case E_APP:
    freps_expr(e->fn, P, slot);
    for (int i = 0; i < e->n; i++) freps_expr(e->args[i], P, slot);
    return;
  case E_LAM: freps_lambda(e->lam, P, slot); return;
  case E_LET:
    freps_group(&e->g, P, slot);
    freps_expr(e->body, P, slot);
    return;
  case E_IF:
    freps_expr(e->cond, P, slot);
    freps_expr(e->then_, P, slot);
    freps_expr(e->else_, P, slot);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) freps_expr(e->scruts[i], P, slot);
    for (int a = 0; a < e->na; a++) {
      Rhs *r = &e->alts[a]->rhs;
      for (int i = 0; i < r->nwg; i++) freps_group(&r->wg[i], P, slot);
      for (int i = 0; i < r->ng; i++) {
        if (r->guards[i]) freps_expr(r->guards[i], P, slot);
        freps_expr(r->bodies[i], P, slot);
      }
    }
    return;
  case E_ANN: freps_expr(e->body, P, slot); return;
  default: return;
  }
}

static void freps_lambda(Lambda *L, Lambda *P, Rep *pslot) {
  if (L->nfree) {
    Rep *r = xmalloc((size_t)L->nfree * sizeof *r);
    for (int i = 0; i < L->nfree; i++) r[i] = L->free[i].vk == VK_LOCAL ? pslot[L->free[i].idx] : frep(P, L->free[i].idx);
    freps[L->id] = r;
  }
  freps_expr(L->body, L, lam_slots(L));
}

static void cg_lambda(SB *out, Lambda *L) {
  Rep *slot = lam_slots(L);
  SB body = {0};
  sb_puts(&body, "");
  bool trmc = has_trmc(L, L->body);
  Rep rr = trmc ? R_V : result_rep(L);
  CG c = {&body, L, 0, 0, false, slot, trmc, NULL, rr, false};
  cg_tail(&c, L->body);
  for (char *s; !c.reused && (s = strstr(body.s, SHADE_DST));) memmove(s, s + strlen(SHADE_DST), strlen(s + strlen(SHADE_DST)) + 1);
  char *nm = xsprintf("%s", L->name ? L->name : "lambda");
  for (char *s = nm; (s = strstr(s, "*/"));) s[1] = '|'; /* keep the comment closed */
  sb_printf(out, "static %s w_%d(V self", ctype[rr], L->id);
  for (int i = 0; i < L->n; i++) sb_printf(out, ", %s p%d", ctype[param_rep(L, i)], i);
  sb_printf(out, ") { /* %s */\n", nm);
  for (int i = 0; i < L->nslots; i++)
    sb_printf(out, "  %s l%d = %s;\n", ctype[slot[i]], i, i < L->n ? conv(tv(xsprintf("p%d", i), param_rep(L, i)), slot[i]) : "0");
  if (c.trmc) sb_puts(out, "  V head = 0, *dst = &head;\n");
  if (c.looped) sb_puts(out, "  top:;\n");
  sb_puts(out, body.s);
  sb_puts(out, "}\n");
  sb_printf(out, "static V fn_%d(V self, V *a) { return %s; }\n\n", L->id, as_v(tv(wrapper_call(L), rr)));
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

/* A foreign export's C declaration: its declared types, as C's. */
static const char *export_head(ForeignDecl *f) {
  SB b = {0};
  Type *t = repr(f->xtype);
  for (int i = 0; i < f->arity; i++, t = repr(t->args[1]))
    if (ffi_kind(t->args[0]) != 'v') sb_printf(&b, "%s%s x%d", b.n ? ", " : "", ffi_param_type(t->args[0]), i);
  return xsprintf("%s %s(%s)", ffi_param_type(t), f->cname, b.n ? b.s : "void");
}

/* It calls the function's typed worker; check_export_code has checked
   that the worker takes and returns C scalars or addresses. */
static void export_body(SB *out, ForeignDecl *f) {
  Lambda *L = globals.a[f->gidx].lam;
  SB call = {0};
  sb_printf(&call, "w_%d(G[%d]", L->id, f->gidx);
  Type *t = repr(f->xtype);
  for (int i = 0; i < f->arity; i++, t = repr(t->args[1]))
    sb_printf(&call, ", %s", ffi_kind(t->args[0]) == 'v' ? "fk_unit" : xsprintf("(%s)x%d", ctype[param_rep(L, i)], i));
  sb_puts(&call, ")");
  bool unit = ffi_kind(t) == 'v';
  sb_printf(out, "%s { /* foreign export %s */\n  %s%s;\n}\n", export_head(f), f->name,
            unit ? "(void)" : xsprintf("return (%s)", ffi_param_type(t)), call.s);
}

char *cgen_program(Chunk *chunks, int nchunks) {
  Reach r = reach_program(chunks, nchunks);
  for (int i = 0; i < ffi_exports.n; i++) reach_global(&r, ffi_exports.a[i]->gidx);
  SB out = {0};
  sb_puts(&out, fk_rt_src);
  VEC(ForeignDecl *) fs = {0};
  for (int i = 0; i < globals.n; i++)
    if (globals.a[i].ffi && r.global[i]) VPUSH(fs, globals.a[i].ffi);
  for (int i = 0; i < ffi_exports.n; i++) /* before the glue, which may take an export's address */
    sb_printf(&out, "%s;\n", export_head(ffi_exports.a[i]));
  sb_puts(&out, ffi_glue(fs.a, fs.n, false));
  sb_printf(&out, "\n/* ---- generated program ---- */\n\nstatic V G[%d];\nstatic size_t G_n = %d;\n\n",
            globals.n ? globals.n : 1, globals.n);
  for (int i = 0; i < all_lambdas.n; i++)
    if (r.lam[all_lambdas.a[i]->id]) {
      Lambda *L = all_lambdas.a[i];
      Rep rr = has_trmc(L, L->body) ? R_V : result_rep(L);
      sb_printf(&out, "static V fn_%d(V self, V *a);\nstatic %s w_%d(V self", L->id, ctype[rr], L->id);
      for (int j = 0; j < L->n; j++) sb_printf(&out, ", %s", ctype[param_rep(L, j)]);
      sb_puts(&out, ");\n");
    }
  sb_puts(&out, "\n");
  int maxid = 0;
  for (int i = 0; i < all_lambdas.n; i++) maxid = all_lambdas.a[i]->id > maxid ? all_lambdas.a[i]->id : maxid;
  freps = xcalloc((size_t)maxid + 1, sizeof *freps);
  for (int i = 0; i < all_lambdas.n; i++) /* the outermost functions; nested ones from their creators */
    if (r.lam[all_lambdas.a[i]->id] && !all_lambdas.a[i]->nfree) freps_lambda(all_lambdas.a[i], NULL, NULL);
  for (int i = 0; i < all_lambdas.n; i++)
    if (r.lam[all_lambdas.a[i]->id]) cg_lambda(&out, all_lambdas.a[i]);
  for (int i = 0; i < ffi_exports.n; i++) export_body(&out, ffi_exports.a[i]);
  sb_puts(&out, "static void program(void *arg) {\n  static V *roots = G;\n  fk_add_roots(&roots, &G_n);\n");
  for (uint32_t i = FK_NBUILTIN_CONS; i < fk_ncons; i++) {
    sb_printf(&out, "  fk_register_con(\"%s\", %d);\n", fk_cons[i].name, fk_cons[i].arity);
    if (con_raw((int)i)) sb_printf(&out, "  fk_con_raw(%u, %uu);\n", i, con_raw((int)i));
    if (!fk_cons[i].labels) continue;
    sb_puts(&out, "  { static const char *const ls[] = {");
    for (int j = 0; j < fk_cons[i].arity; j++) sb_printf(&out, "%s\"%s\"", j ? ", " : "", fk_cons[i].labels[j]);
    sb_printf(&out, "}; fk_con_labels(%u, ls); }\n", i);
  }
  for (int i = 0; i < globals.n; i++)
    if (globals.a[i].prim >= 0 && r.global[i]) sb_printf(&out, "  G[%d] = fk_prim_value(%d);\n", i, globals.a[i].prim);
  for (int i = 0; i < globals.n; i++)
    if (globals.a[i].ffi && r.global[i]) {
      int n = globals.a[i].ffi->arity; /* 0: a C value, read once */
      if (n) sb_printf(&out, "  G[%d] = fk_closure(fk_ffi_%d, %d, 0, 0);\n", i, i, n);
      else sb_printf(&out, "  G[%d] = fk_ffi_%d(0, 0);\n", i, i);
    }
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
