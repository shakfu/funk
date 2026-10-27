/* check.c -- dependency analysis and Hindley-Milner type inference.

   Inference is algorithm J with mutable unification variables and
   levels for generalization (Remy; as in OCaml). Generalized variables are
   marked in place with level GENERIC; instantiation copies them.

   Classes: a type variable carries the set of classes it must be an
   instance of (`cls`, closed under superclasses). Binding it to `T args`
   requires an instance for each class, and the instance context adds
   classes to `args`. A generalized variable keeps its classes, which gives
   schemes `Eq a => ...`; elab.c later turns each constraint into a
   dictionary argument. Integer literals get a fresh `Num` variable.

   Monomorphism restriction: a binding that is not syntactically a lambda
   does not generalize its constrained variables, so it is not recomputed
   per use. Local bindings without a signature never generalize them; see
   docs/dev/type-classes.md. They stay open until a later use fixes them. At the end of the
   chunk a remaining `Num` variable defaults to Int; any other constrained
   variable is ambiguous.

   Records: construction and record patterns name their constructor, so
   inference lowers them to positional form directly. Field access `p.x`
   and update `p { x = e }` need the type of `p`, which may be fixed only
   later in the group (`map (.x) ps`). They wait in `pending` and are
   lowered to `case` before the group is generalized; one still open then
   is an error. See docs/dev/records.md. */
#include <ctype.h>
#include <limits.h>

#include "funk.h"

#define GENERIC INT_MAX
#define NBUILTIN_TYPES 9 /* Int Float Char Bool () [] -> Num String */

__typeof__(globals) globals;
__typeof__(condefs) condefs;
__typeof__(tydefs) tydefs;
__typeof__(all_lambdas) all_lambdas;
__typeof__(classes) classes;
__typeof__(insts) insts;

static Map gmap, cmap, tmap, clsmap;
static int level;
static int tvid;
static VEC(Type *) cvars; /* variables that gained classes, for defaulting */
static uint64_t num_bit;  /* the Num class, once the prelude declares it */
int num_class = -1;
Type *dict_type;
static const char *s_int, *s_float, *s_char, *s_bool, *s_list, *s_arrow, *s_unit, *s_string, *s_otherwise;

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
    copy_n(t->args, args, (size_t)n * sizeof *args);
  }
  return t;
}

static Type *tcon0(const char *name) { return tcon(name, 0, NULL); }
Type *tcon_new(const char *name, int n, Type **args) { return tcon(name, n, args); }

/* Effect arrows (typed-effects.md, option E). Every arrow carries an
   effect: Pure, Eff or a variable; `a ->> b` is written Eff, and the `->`
   arrows of one signature share a variable. A call acts only through the
   arrow that saturates a function of known arity, and a call of a
   function whose type is not yet known is settled at the end of its
   binding group. */
static Type *eff_pure, *eff_eff, *cur_eff, *te_eff;
bool type_acts(Type *t) {
  for (t = repr(t); t->k == TY_CON && t->name == s_arrow; t = repr(t->args[1]))
    if (t->eff && repr(t->eff) == eff_eff) return true;
  return false;
}
static const char *cur_bind;
static int nprelude; /* globals defined by the prelude and the primitives: their names are reserved */
static int nprelude_types = NBUILTIN_TYPES, nprelude_cons = FK_NBUILTIN_CONS; /* likewise, types and constructors */
static Type *newvar(void);

static Type *tarrow(Type *a, Type *b) {
  Type *t = tcon(s_arrow, 2, (Type *[]){a, b});
  t->eff = newvar();
  return t;
}

static Type *gvar(uint64_t cls);

/* A constructor never acts: its own arrows are polymorphic in their effect. */
static void con_effects(Type *t) {
  for (t = repr(t); t->k == TY_CON && t->name == s_arrow; t = repr(t->args[1])) t->eff = gvar(0);
}
static Type *tlist(Type *a) { return tcon(s_list, 1, &a); }

static Type *newvar(void) {
  Type *t = NEW(Type);
  t->k = TY_VAR;
  t->level = level;
  t->id = tvid++;
  return t;
}

static Type *numvar(int line, int col) {
  Type *t = newvar();
  t->cls = num_bit;
  t->line = line;
  t->col = col;
  if (num_bit) VPUSH(cvars, t);
  return t;
}

static Type *gvar(uint64_t cls) {
  Type *t = newvar();
  t->level = GENERIC;
  t->cls = cls;
  return t;
}

#define BIT(c) ((uint64_t)1 << (c))

static uint64_t closure_of(uint64_t m) {
  uint64_t r = 0;
  for (int c = 0; c < classes.n; c++)
    if (m & BIT(c)) r |= classes.a[c].closure;
  return r;
}

/* Drop classes implied by another class in m: {Eq, Ord} -> {Ord}. */
static uint64_t minimal(uint64_t m) {
  uint64_t r = m;
  for (int c = 0; c < classes.n; c++)
    if (m & BIT(c)) r &= ~(classes.a[c].closure & ~BIT(c));
  return r;
}

InstDef *find_inst(int cls, const char *tycon) {
  for (int i = 0; i < insts.n; i++)
    if (insts.a[i].cls == cls && insts.a[i].tycon == tycon) return &insts.a[i];
  return NULL;
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

/* Name of the i-th variable seen in a message. A rigid variable keeps its
   name from the signature; the others are lettered, skipping those names. */
static const char *var_name(Names *nm, int i) {
  if (nm->a[i]->skolem && nm->a[i]->vname) return nm->a[i]->vname;
  int k = 0;
  for (int j = 0; j <= i; j++) {
    if (nm->a[j]->skolem && nm->a[j]->vname) continue;
    for (;;) {
      const char *s = k < 26 ? xsprintf("%c", 'a' + k) : xsprintf("t%d", k);
      k++;
      bool taken = false;
      for (int m = 0; m < nm->n; m++)
        taken |= nm->a[m]->skolem && nm->a[m]->vname && strcmp(nm->a[m]->vname, s) == 0;
      if (!taken) {
        if (j == i) return s;
        break;
      }
    }
  }
  abort();
}

static void pr(SB *b, Type *t, int prec, Names *nm) {
  t = repr(t);
  if (t->k == TY_VAR) {
    int i = 0;
    while (i < nm->n && nm->a[i] != t) i++;
    if (i == nm->n) VPUSH(*nm, t);
    sb_puts(b, var_name(nm, i));
    return;
  }
  if (t->name == s_arrow) {
    if (prec > 0) sb_puts(b, "(");
    pr(b, t->args[0], 1, nm);
    sb_puts(b, t->eff && repr(t->eff) == eff_eff ? " ->> " : " -> ");
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
  SB ctx = {0}, cs = {0};
  sb_puts(&ctx, "");
  int nc = 0;
  for (int i = 0; i < nm.n; i++) {
    uint64_t m = minimal(nm.a[i]->cls);
    for (int c = 0; c < classes.n; c++) {
      if (!(m & BIT(c))) continue;
      sb_printf(&cs, "%s%s %s", nc++ ? ", " : "", classes.a[c].name, var_name(&nm, i));
    }
  }
  if (nc) sb_printf(&ctx, nc > 1 ? "(%s) => " : "%s => ", cs.s);
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
  if (t->eff && repr(t->eff)->k == TY_VAR && repr(t->eff)->level > v->level && !repr(t->eff)->skolem)
    repr(t->eff)->level = v->level;
}

static void entail(Type *t, uint64_t m);

/* v must be an instance of the classes in m. */
static void add_classes(Type *v, uint64_t m, int line, int col) {
  m = closure_of(m);
  if (!(m & ~v->cls)) return;
  if (v->skolem) {
    Names nm = {0};
    int c = 0;
    while (!(m & ~v->cls & BIT(c))) c++;
    const char *s = tstr(v, &nm);
    cerror(uline, ucol, "no instance `%s %s` for a rigid type variable; add `%s %s` to the signature",
           classes.a[c].name, s, classes.a[c].name, s);
  }
  v->cls |= m;
  if (!v->line) {
    v->line = line;
    v->col = col;
  }
  VPUSH(cvars, v);
}

/* The constructor type t must be an instance of every class in m. */
static void entail(Type *t, uint64_t m) {
  for (int c = 0; c < classes.n; c++) {
    if (!(m & BIT(c))) continue;
    InstDef *in = find_inst(c, t->name);
    if (!in) {
      Names nm = {0};
      const char *s = tstr(t, &nm);
      bool paren = t->n && !is_tuple_name(t->name) && t->name != s_list;
      cerror(uline, ucol, paren ? "no instance `%s (%s)`" : "no instance `%s %s`", classes.a[c].name, s);
    }
    for (int i = 0; i < in->arity; i++) {
      uint64_t need = in->vars[i]->cls;
      if (!need) continue;
      Type *a = repr(t->args[i]);
      if (a->k == TY_VAR) add_classes(a, need, uline, ucol);
      else entail(a, closure_of(need));
    }
  }
}

static void bind_var(Type *v, Type *t) {
  if (t->k == TY_VAR) {
    add_classes(t, v->cls, v->line, v->col);
    if (t->skolem) {
      if (t->level > v->level) escape_error();
    } else if (v->level < t->level) {
      t->level = v->level;
    }
    v->link = t;
    return;
  }
  if (v->cls) entail(t, v->cls);
  occurs_adjust(v, t);
  v->link = t;
}

static _Noreturn void eff_error(const char *msg) {
  const char *b = cur_bind;
  if (b && !strncmp(b, "$m.", 3)) { /* an instance method: $m.Class.Type.method */
    const char *cls = b + 3, *ty = strchr(cls, '.'), *meth = strrchr(b, '.');
    if (ty && meth > ty)
      cerror(uline, ucol, "in `%s` of `instance %.*s %.*s`: %s", meth + 1, (int)(ty - cls), cls, (int)(meth - ty - 1), ty + 1, msg);
  }
  if (b && b[0] != '$') cerror(uline, ucol, "in `%s`: %s", b, msg);
  cerror(uline, ucol, "%s", msg);
}

/* Unify two effects. */
static void unify_eff(Type *a, Type *b) {
  a = repr(a);
  b = repr(b);
  if (a == b) return;
  if (b->k == TY_VAR && !b->skolem) { Type *t = a; a = b; b = t; }
  if (a->k == TY_VAR && !a->skolem) {
    if (b->k == TY_VAR && !b->skolem && b->level > a->level) b->level = a->level;
    a->link = b;
    return;
  }
  if (a == eff_eff || b == eff_eff) eff_error("an effectful function (`->>`) where a pure one (`->`) is required");
  eff_error("functions whose effects differ: a signature fixes one, and another is required");
}

static void unify_rec(Type *a, Type *b) {
  a = repr(a);
  b = repr(b);
  if (a == b) return;
  if (a->k == TY_VAR && !a->skolem) { bind_var(a, b); return; }
  if (b->k == TY_VAR && !b->skolem) { bind_var(b, a); return; }
  if (a->k == TY_CON && b->k == TY_CON && a->name == b->name && a->n == b->n) {
    for (int i = 0; i < a->n; i++) unify_rec(a->args[i], b->args[i]);
    if (a->eff && b->eff) unify_eff(a->eff, b->eff);
    return;
  }
  mismatch(a, b);
}

/* Calls of a function whose type was unknown when the call was inferred,
   such as a record field resolved later: their effect is settled at the
   end of the binding group, once the type is known. */
typedef struct { Type *eff, *ctx; int line, col; const char *bind; } Deferred;
static VEC(Deferred) deferred;

/* A local value binding whose effect is tied to something outside it,
   such as a recursive call of the function being inferred, is decided once
   its top-level binding is general, against the parameter types of the
   functions around it. */
typedef VEC(Type *) Types;
typedef struct { Bind *b; Type *eff; Types params; const char *in; } LocalEff;
static VEC(LocalEff) local_effs;
static Types param_stack; /* parameter types of the lambdas being checked */

static void call_effect(Type *e);

static void settle_deferred(int from) {
  Type *saved = cur_eff;
  const char *sb = cur_bind;
  for (int i = from; i < deferred.n; i++) {
    Deferred *d = &deferred.a[i];
    cur_eff = d->ctx;
    cur_bind = d->bind;
    uline = d->line;
    ucol = d->col;
    call_effect(d->eff); /* Pure: nothing; Eff or still unknown: as an immediate call */
  }
  deferred.n = from;
  cur_eff = saved;
  cur_bind = sb;
}

/* A call through an arrow with effect e, in code whose effect is cur_eff. */
static void call_effect(Type *e) {
  if (!e || !cur_eff) return;
  e = repr(e);
  if (e == eff_pure) return; /* a pure call constrains nothing */
  Type *c = repr(cur_eff);
  if (e == eff_eff && c->skolem) eff_error("declared with `->` but calls an effectful function; its signature needs `->>`");
  if (e == eff_eff && c == eff_pure)
    eff_error("calls an effectful function, but must be pure: a data field or class method declared with `->`");
  unify_eff(c, e);
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
    v->cls = t->cls;
    v->skolem = skolem;
    v->vname = t->vname;
    if (v->cls && !skolem) VPUSH(cvars, v);
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
  Type *eff = t->eff ? inst_rec(t->eff, s, skolem) : NULL;
  same &= eff == t->eff;
  if (same) { free(args); return t; }
  Type *r = tcon(t->name, t->n, args);
  r->eff = eff;
  free(args);
  return r;
}

static Type *inst(Type *t, bool skolem) {
  Subst s = {0};
  Type *r = inst_rec(t, &s, skolem);
  free(s.a);
  return r;
}

/* Generic constrained variables of a scheme, one entry per class in
   canonical order: variables by first occurrence, classes by id, implied
   superclasses dropped. Definitions and uses agree on this order. */
static void collect_generic(Type *t, Names *vs) {
  t = repr(t);
  if (t->k == TY_VAR) {
    if (t->level != GENERIC || !t->cls) return;
    for (int i = 0; i < vs->n; i++)
      if (vs->a[i] == t) return;
    VPUSH(*vs, t);
    return;
  }
  for (int i = 0; i < t->n; i++) collect_generic(t->args[i], vs);
}

int preds_of(Type *scheme, Pred **out) {
  Names vs = {0};
  VEC(Pred) ps = {0};
  if (scheme) collect_generic(scheme, &vs);
  for (int i = 0; i < vs.n; i++) {
    uint64_t m = minimal(vs.a[i]->cls);
    for (int c = 0; c < classes.n; c++)
      if (m & BIT(c)) VPUSH(ps, ((Pred){vs.a[i], c}));
  }
  free(vs.a);
  *out = ps.a;
  return ps.n;
}

/* Instantiate the scheme of variable e and record the instantiated
   constraints on e, for elaboration. */
static Type *inst_var(Type *scheme, Expr *e) {
  Pred *ps;
  int n = preds_of(scheme, &ps);
  Subst s = {0};
  Type *t = inst_rec(scheme, &s, false);
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < s.n; k += 2)
      if (s.a[k] == ps[i].t) ps[i].t = s.a[k + 1];
    if (!ps[i].t->line) {
      ps[i].t->line = e->line;
      ps[i].t->col = e->col;
    }
  }
  free(s.a);
  e->preds = ps;
  e->npreds = n;
  return t;
}

static void gen(Type *t, bool mr) {
  t = repr(t);
  if (t->k == TY_VAR) {
    if (!t->skolem && t->level != GENERIC && t->level > level) t->level = mr && t->cls ? level : GENERIC;
    return;
  }
  for (int i = 0; i < t->n; i++) gen(t->args[i], mr);
  if (t->eff) gen(t->eff, false);
}

/* --------------------------------------------------------- type syntax */

typedef struct { Strs names; VEC(Type *) types; } TVars;

static Type *te_type(TypeExpr *te, TVars *tv, Sig *sig) {
  switch (te->k) {
  case TE_VAR:
    for (int i = 0; i < tv->names.n; i++)
      if (tv->names.a[i] == te->name) return tv->types.a[i];
    if (!sig) cerror(te->line, te->col, "type variable `%s` is not a parameter of the data type", te->name);
    {
      uint64_t m = 0;
      for (int i = 0; i < sig->nctx; i++) {
        if (sig->ctx[i] != te->name) continue;
        int c = map_get(&clsmap, sig->ctx_cls[i]);
        if (c < 0) cerror(sig->line, sig->col, "unknown class `%s`", sig->ctx_cls[i]);
        m |= classes.a[c].closure;
      }
      Type *v = gvar(m);
      v->vname = te->name;
      VPUSH(tv->names, te->name);
      VPUSH(tv->types, v);
      return v;
    }
  case TE_FUN: {
    Type *t = tarrow(te_type(te->args[0], tv, sig), te_type(te->args[1], tv, sig));
    t->eff = te->name && !strcmp(te->name, "->>") ? eff_eff : te_eff ? te_eff : eff_pure;
    return t;
  }
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
  Type *saved = te_eff;
  te_eff = gvar(0); /* the `->` arrows of a signature share one effect */
  Type *t = te_type(s->t, &tv, s);
  te_eff = saved;
  for (int i = 0; i < s->nctx; i++) {
    bool found = false;
    for (int k = 0; k < tv.names.n; k++) found |= tv.names.a[k] == s->ctx[i];
    if (!found) cerror(s->line, s->col, "constraint on `%s`, which is not in the type", s->ctx[i]);
  }
  return t;
}

/* ----------------------------------------------------------------- data */

static bool type_eq(Type *a, Type *b);

typedef struct { const char *label, *con; Type *t; } FieldSeen;

static void register_datas(Module *m) {
  for (int i = 0; i < m->nd; i++) {
    DataDecl *d = m->datas[i];
    for (int j = 0; j < i; j++)
      if (m->datas[j]->name == d->name) cerror(d->line, d->col, "duplicate data type `%s`", d->name);
    int ti = map_get(&tmap, d->name);
    if (ti >= 0 && ti < nprelude_types)
      cerror(d->line, d->col, "type `%s` is defined in the prelude and cannot be redefined", d->name);
    for (int j = 0; j < m->nstructs; j++)
      if (m->structs[j]->name == d->name) cerror(d->line, d->col, "type `%s` is already defined", d->name);
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
      VPUSH(tv.types, gvar(0));
    }
    Type *res = tcon(d->name, d->np, tv.types.a);
    VEC(FieldSeen) seen = {0};
    for (int c = 0; c < d->nc; c++) {
      ConDecl *cd = &d->cons[c];
      int prev = map_get(&cmap, cd->name);
      if (prev >= (int)FK_NBUILTIN_CONS && condefs.a[prev].tydef >= tydefs.n - m->nd)
        cerror(cd->line, cd->col, "duplicate constructor `%s`", cd->name);
      if (prev >= 0 && prev < nprelude_cons)
        cerror(cd->line, cd->col, "constructor `%s` is defined in the prelude and cannot be redefined", cd->name);
      Type *t = res;
      for (int f = cd->nfields; f-- > 0;) {
        Type *ft = te_type(cd->fields[f], &tv, NULL);
        t = tarrow(ft, t);
        if (!cd->labels) continue;
        for (int k = 0; k < seen.n; k++) {
          if (seen.a[k].label != cd->labels[f]) continue;
          if (seen.a[k].con == cd->name)
            cerror(cd->line, cd->col, "duplicate field `%s` in `%s`", cd->labels[f], cd->name);
          if (!type_eq(seen.a[k].t, ft))
            cerror(cd->line, cd->col, "field `%s` has different types in `%s` and `%s`", cd->labels[f],
                   seen.a[k].con, cd->name);
        }
        VPUSH(seen, ((FieldSeen){cd->labels[f], cd->name, ft}));
      }
      uint32_t id = fk_register_con(cd->name, cd->nfields);
      if ((int)id != condefs.n) abort();
      con_effects(t);
      fk_con_labels(id, cd->labels);
      VPUSH(condefs, ((ConDef){cd->name, cd->nfields, t, map_get(&tmap, d->name), cd->labels}));
      map_put(&cmap, cd->name, (int)id);
    }
    free(seen.a);
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
  case E_FIELD: /* p.x may call any Type.x defined in this group */
    deps_expr(e->body, sh, d);
    for (int i = 0; i < d->n; i++) {
      const char *dot = strchr(d->b[i]->name, '.');
      if (isupper((unsigned char)d->b[i]->name[0]) && dot && strcmp(dot + 1, e->name) == 0)
        d->adj[d->cur * d->n + i] = true;
    }
    return;
  case E_RECORD:
    deps_expr(e->fn, sh, d);
    for (int i = 0; i < e->n; i++) deps_expr(e->args[i], sh, d);
    return;
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
  case E_ANN:
  case E_FIELD: split_expr(e->body); return;
  case E_RECORD:
    split_expr(e->fn);
    for (int i = 0; i < e->n; i++) split_expr(e->args[i]);
    return;
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

/* ---------------------------------------------------------------- records */

typedef struct { Expr *e; Type *rec, *res; Type **vals; } Pending;
static VEC(Pending) pending;
static int rec_counter;

static int first_con(int ti) {
  int c = 0;
  while (condefs.a[c].tydef != ti) c++;
  return c;
}

static int label_index(int con, const char *name) {
  const char **ls = condefs.a[con].labels;
  for (int i = 0; ls && i < condefs.a[con].arity; i++)
    if (ls[i] == name) return i;
  return -1;
}

static bool has_label(int ti, const char *name) {
  for (int c = first_con(ti), k = 0; k < tydefs.a[ti].ncons; k++)
    if (label_index(c + k, name) >= 0) return true;
  return false;
}

/* Type of field i of constructor con, for a value of type rec. */
static Type *field_type(int con, int i, Type *rec, int line, int col) {
  Type *t = inst(condefs.a[con].scheme, false), *fi = NULL;
  for (int k = 0; k < condefs.a[con].arity; k++) {
    t = repr(t);
    if (k == i) fi = t->args[0];
    t = t->args[1];
  }
  unify(t, rec, line, col);
  return fi;
}

static Expr *ex_new(int k, int line, int col) {
  Expr *e = NEW(Expr);
  e->k = k;
  e->line = line;
  e->col = col;
  return e;
}

static Expr *ex_var(const char *name, int line, int col) {
  Expr *e = ex_new(E_VAR, line, col);
  e->name = name;
  return e;
}

static Expr *ex_app(Expr *f, int n, Expr **args) {
  Expr *e = ex_new(E_APP, f->line, f->col);
  e->fn = f;
  e->n = n;
  e->args = args;
  return e;
}

/* error msg; `$error` is an alias that user code cannot shadow. */
static Expr *ex_error(const char *msg, int line, int col) {
  Expr *s = ex_new(E_STR, line, col);
  s->name = msg;
  s->slen = (int)strlen(msg);
  s->ty = tcon0(s_string);
  Expr **a = xmalloc(sizeof *a);
  a[0] = s;
  Expr *f = ex_var(intern("$error"), line, col);
  f->ty = dict_type; /* a function value: represented as V */
  return ex_app(f, 1, a); /* the caller sets the result type */
}

/* Constructor pattern with wildcard arguments. */
static Pat *con_pat(int con, int line, int col) {
  Pat *p = NEW(Pat);
  *p = (Pat){.k = P_CON, .line = line, .col = col, .name = condefs.a[con].name, .con = con};
  p->n = condefs.a[con].arity;
  p->args = xmalloc((size_t)p->n * sizeof *p->args + 1);
  for (int i = 0; i < p->n; i++) {
    p->args[i] = NEW(Pat);
    *p->args[i] = (Pat){.k = P_WILD, .line = line, .col = col};
  }
  return p;
}

static Pat *var_pat(const char *name, int line, int col) {
  Pat *p = NEW(Pat);
  *p = (Pat){.k = P_VAR, .line = line, .col = col, .name = name};
  return p;
}

static Alt *alt1(Pat *p, Expr *body) {
  Alt *a = NEW(Alt);
  a->np = 1;
  a->pats = xmalloc(sizeof *a->pats);
  a->pats[0] = p;
  a->rhs.ng = 1;
  a->rhs.guards = xcalloc(1, sizeof(Expr *));
  a->rhs.bodies = xmalloc(sizeof(Expr *));
  a->rhs.bodies[0] = body;
  return a;
}

/* Overwrite e with `case scrut of alts`. */
static void make_case(Expr *e, Expr *scrut, int na, Alt **alts) {
  Expr **sc = xmalloc(sizeof *sc);
  sc[0] = scrut;
  e->k = E_CASE;
  e->name = cur_file;
  e->ns = 1;
  e->scruts = sc;
  e->na = na;
  e->alts = alts;
}

/* C { f = e, ... } -> C e1 .. en */
static void lower_construction(Expr *e) {
  Expr *fn = e->fn;
  int c = con_lookup(fn->name, fn->line, fn->col);
  ConDef *cd = &condefs.a[c];
  Expr **args = xcalloc((size_t)cd->arity + 1, sizeof *args);
  for (int k = 0; k < e->n; k++) {
    int i = label_index(c, e->fields[k]);
    if (i < 0) cerror(e->args[k]->line, e->args[k]->col, "constructor `%s` has no field `%s`", cd->name, e->fields[k]);
    if (args[i]) cerror(e->args[k]->line, e->args[k]->col, "field `%s` is given twice", e->fields[k]);
    args[i] = e->args[k];
  }
  for (int i = 0; i < cd->arity; i++) {
    if (args[i]) continue;
    if (!cd->labels) cerror(e->line, e->col, "constructor `%s` has no field names; give its arguments in order", cd->name);
    cerror(e->line, e->col, "missing field `%s` in construction of `%s`", cd->labels[i], cd->name);
  }
  if (!cd->arity) {
    *e = *fn;
    return;
  }
  e->k = E_APP;
  e->n = cd->arity;
  e->args = args;
  e->fields = NULL;
}

/* C { f = p } -> C _ .. p .. _ */
static void lower_rec_pat(Pat *p, int con) {
  ConDef *cd = &condefs.a[con];
  Pat **args = xcalloc((size_t)cd->arity + 1, sizeof *args);
  for (int k = 0; k < p->n; k++) {
    int i = label_index(con, p->fields[k]);
    if (i < 0) cerror(p->args[k]->line, p->args[k]->col, "constructor `%s` has no field `%s`", cd->name, p->fields[k]);
    if (args[i]) cerror(p->args[k]->line, p->args[k]->col, "field `%s` is matched twice", p->fields[k]);
    args[i] = p->args[k];
  }
  for (int i = 0; i < cd->arity; i++) {
    if (args[i]) continue;
    args[i] = NEW(Pat);
    *args[i] = (Pat){.k = P_WILD, .line = p->line, .col = p->col};
  }
  p->n = cd->arity;
  p->args = args;
  p->rec = false;
}

/* p.x on type ti: one alternative per constructor. */
static void lower_field(Pending *pd, int ti) {
  Expr *e = pd->e;
  const char *x = e->name, *v = intern("$f");
  int first = first_con(ti), nc = tydefs.a[ti].ncons;
  Alt **alts = xmalloc((size_t)nc * sizeof *alts);
  Type *ft = NULL;
  for (int c = first; c < first + nc; c++) {
    Pat *p = con_pat(c, e->line, e->col);
    int i = label_index(c, x);
    Expr *body;
    if (i >= 0) {
      if (!ft) ft = field_type(c, i, pd->rec, e->line, e->col);
      p->args[i] = var_pat(v, e->line, e->col);
      p->args[i]->ty = ft;
      body = ex_var(v, e->line, e->col);
      body->ty = ft;
    } else {
      body = ex_error(xsprintf("constructor `%s` has no field `%s`", condefs.a[c].name, x), e->line, e->col);
      body->ty = pd->res;
    }
    p->ty = pd->rec;
    alts[c - first] = alt1(p, body);
  }
  unify(pd->res, ft, e->line, e->col);
  make_case(e, e->body, nc, alts);
  e->ty = pd->res;
}

/* r { f = v, ... }: rebuild each constructor that has every field; bind the
   values first when several constructors would each mention them. */
static void lower_update(Pending *pd, int ti) {
  Expr *e = pd->e;
  int first = first_con(ti), nc = tydefs.a[ti].ncons, nok = 0, ok1 = -1;
  for (int k = 0; k < e->n; k++) {
    if (!has_label(ti, e->fields[k]))
      cerror(e->args[k]->line, e->args[k]->col, "type `%s` has no field `%s`", tydefs.a[ti].name, e->fields[k]);
    for (int j = 0; j < k; j++)
      if (e->fields[j] == e->fields[k]) cerror(e->args[k]->line, e->args[k]->col, "field `%s` is given twice", e->fields[k]);
  }
  const char **missing = xcalloc((size_t)nc, sizeof *missing);
  for (int c = first; c < first + nc; c++) {
    for (int k = 0; k < e->n && !missing[c - first]; k++)
      if (label_index(c, e->fields[k]) < 0) missing[c - first] = e->fields[k];
    if (missing[c - first]) continue;
    nok++;
    if (ok1 < 0) ok1 = c;
  }
  if (!nok) cerror(e->line, e->col, "no constructor of `%s` has all of the updated fields", tydefs.a[ti].name);
  for (int k = 0; k < e->n; k++)
    unify(field_type(ok1, label_index(ok1, e->fields[k]), pd->rec, e->line, e->col), pd->vals[k], e->args[k]->line,
          e->args[k]->col);
  Expr **vals = xmalloc((size_t)e->n * sizeof *vals + 1);
  Bind **binds = xmalloc((size_t)e->n * sizeof *binds + 1);
  for (int k = 0; k < e->n; k++) {
    vals[k] = e->args[k];
    if (nok == 1) continue;
    Bind *b = NEW(Bind);
    *b = (Bind){.name = intern(xsprintf("$v%d", rec_counter++)), .e = e->args[k], .line = e->line, .col = e->col};
    binds[k] = b;
  }
  Alt **alts = xmalloc((size_t)nc * sizeof *alts);
  for (int c = first; c < first + nc; c++) {
    Pat *p = con_pat(c, e->line, e->col);
    Expr *body;
    p->ty = pd->rec;
    if (missing[c - first]) {
      body = ex_error(xsprintf("constructor `%s` has no field `%s`", condefs.a[c].name, missing[c - first]), e->line,
                      e->col);
      body->ty = pd->rec;
    } else {
      int ar = condefs.a[c].arity;
      Expr **args = xmalloc((size_t)ar * sizeof *args);
      for (int i = 0; i < ar; i++) {
        int k = 0;
        while (k < e->n && label_index(c, e->fields[k]) != i) k++;
        Type *fty = field_type(c, i, pd->rec, e->line, e->col);
        if (k < e->n) {
          args[i] = nok == 1 ? vals[k] : ex_var(binds[k]->name, e->line, e->col);
          args[i]->ty = fty;
        } else {
          const char *u = intern(xsprintf("$u%d", i));
          p->args[i] = var_pat(u, e->line, e->col);
          p->args[i]->ty = fty;
          args[i] = ex_var(u, e->line, e->col);
          args[i]->ty = fty;
        }
      }
      Expr *con = ex_new(E_CON, e->line, e->col);
      con->name = condefs.a[c].name;
      con->idx = c;
      con->ty = dict_type; /* a function value: represented as V */
      body = ex_app(con, ar, args);
      body->ty = pd->rec;
    }
    alts[c - first] = alt1(p, body);
  }
  Expr *cs = ex_new(E_CASE, e->line, e->col);
  make_case(cs, e->fn, nc, alts);
  cs->ty = pd->rec;
  for (int k = nok == 1 ? 0 : e->n; k-- > 0;) {
    binds[k]->type = pd->vals[k];
    Expr *let = ex_new(E_LET, e->line, e->col);
    let->ty = pd->rec;
    let->g = (Group){xmalloc(sizeof(Bind *)), 1, false};
    let->g.b[0] = binds[k];
    let->body = cs;
    cs = let;
  }
  *e = *cs;
  free(missing);
}

/* p.x where x is not a field: a function Type.x applied to p. */
static bool lower_method(Pending *pd, Type *r) {
  Expr *e = pd->e;
  const char *q = intern(xsprintf("%s.%s", r->name, e->name));
  int g = map_get(&gmap, q);
  if (g < 0 || !globals.a[g].type) return false;
  Expr *f = ex_var(q, e->line, e->col);
  unify(f->ty = inst_var(globals.a[g].type, f), tarrow(pd->rec, pd->res), e->line, e->col);
  Expr **a = xmalloc(sizeof *a);
  a[0] = e->body;
  *e = *ex_app(f, 1, a);
  e->ty = pd->res;
  return true;
}

static void lower_pending(Pending *pd) {
  Expr *e = pd->e;
  Type *r = repr(pd->rec);
  int ti = map_get(&tmap, r->name);
  if (e->k == E_RECORD) {
    if (ti < 0 || !tydefs.a[ti].ncons) {
      Names nm = {0};
      cerror(e->line, e->col, "record update on `%s`, which has no fields", tstr(r, &nm));
    }
    lower_update(pd, ti);
    return;
  }
  if (ti >= 0 && tydefs.a[ti].ncons && has_label(ti, e->name)) {
    lower_field(pd, ti);
    return;
  }
  if (lower_method(pd, r)) return;
  Names nm = {0};
  cerror(e->line, e->col, "type `%s` has no field or function `%s`%s", tstr(r, &nm), e->name,
         r->name == s_arrow ? "; write `f . g` with spaces for composition" : "");
}

/* Lower every pending access whose record type is now known. */
static void lower_known(void) {
  for (bool progress = true; progress;) {
    progress = false;
    for (int i = 0; i < pending.n; i++) {
      if (repr(pending.a[i].rec)->k != TY_CON) continue;
      Pending pd = pending.a[i];
      pending.a[i--] = pending.a[--pending.n];
      lower_pending(&pd);
      progress = true;
    }
  }
}

static bool is_pending_rec(Type *t) {
  for (int i = 0; i < pending.n; i++)
    if (repr(pending.a[i].rec) == t) return true;
  return false;
}

/* In `Type.f p ... = e` without a signature, a first parameter used with
   dot access but otherwise unconstrained has type Type. */
static void default_receivers(Group *g, Type **mono, Type **sch) {
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    const char *dot = strchr(b->name, '.');
    if (sch[i] || !dot || !isupper((unsigned char)b->name[0]) || b->e->k != E_LAM) continue;
    Type *p0 = repr(repr(mono[i])->args[0]);
    if (p0->k != TY_VAR || !is_pending_rec(p0)) continue;
    const char *tn = intern_n(b->name, (size_t)(dot - b->name));
    int ti = map_get(&tmap, tn);
    Type **vs = xmalloc((size_t)tydefs.a[ti].arity * sizeof *vs + 1);
    for (int k = 0; k < tydefs.a[ti].arity; k++) vs[k] = newvar();
    unify(p0, tcon(tn, tydefs.a[ti].arity, vs), b->line, b->col);
  }
}

/* One pending access still open is an error if its type would be
   generalized at this level. */
static void resolve_fields(bool final) {
  lower_known();
  for (int i = 0; i < pending.n; i++) { /* 21.double: a number defaults to Int, as at the end of a chunk */
    Type *r = repr(pending.a[i].rec);
    if (r->k == TY_VAR && (r->cls & num_bit) && !r->skolem && (final || r->level >= level)) {
      uline = r->line;
      ucol = r->col;
      Type *it = tcon0(s_int);
      entail(it, r->cls);
      r->link = it;
    }
  }
  lower_known();
  for (int i = 0; i < pending.n; i++) {
    Expr *e = pending.a[i].e;
    if (!final && repr(pending.a[i].rec)->level < level) continue;
    if (e->k == E_FIELD) cerror(e->line, e->col, "cannot infer the record type for `.%s`; add a type signature", e->name);
    cerror(e->line, e->col, "cannot infer the record type for this update; add a type signature");
  }
}

/* Type.x where x is a field: \r -> r.x with r of type Type. */
static Type *infer_qualified_field(Expr *e, Env *env) {
  const char *dot = strchr(e->name, '.');
  const char *tn = intern_n(e->name, (size_t)(dot - e->name)), *x = intern(dot + 1);
  int ti = map_get(&tmap, tn);
  if (ti < 0) cerror(e->line, e->col, "unknown type `%s`", tn);
  if (!tydefs.a[ti].ncons || !has_label(ti, x))
    cerror(e->line, e->col, "type `%s` has no field or function `%s`", tn, x);
  Expr *f = ex_new(E_FIELD, e->line, e->col);
  f->name = x;
  f->body = ex_var(intern("$r"), e->line, e->col);
  Lambda *L = NEW(Lambda);
  L->n = 1;
  L->params = xmalloc(sizeof *L->params);
  L->params[0] = f->body->name;
  L->body = f;
  L->line = e->line;
  L->col = e->col;
  e->k = E_LAM;
  e->lam = L;
  Type *t = infer(e, env);
  Type **vs = xmalloc((size_t)tydefs.a[ti].arity * sizeof *vs + 1);
  for (int i = 0; i < tydefs.a[ti].arity; i++) vs[i] = newvar();
  unify(repr(t)->args[0], tcon(tn, tydefs.a[ti].arity, vs), e->line, e->col);
  return t;
}

static Type *infer_pat(Pat *p, Env **env, Env *stop);

static Type *infer_pat0(Pat *p, Env **env, Env *stop) {
  switch (p->k) {
  case P_VAR: {
    for (Env *e = *env; e != stop; e = e->next)
      if (e->name == p->name) cerror(p->line, p->col, "variable `%s` is bound twice in one pattern", p->name);
    Type *t = newvar();
    *env = env_push(*env, p->name, t);
    return t;
  }
  case P_WILD: return newvar();
  case P_INT: return numvar(p->line, p->col);
  case P_CHAR: return tcon0(s_char);
  case P_STR: return tcon0(s_string);
  case P_CON: {
    int id = con_lookup(p->name, p->line, p->col);
    ConDef *c = &condefs.a[id];
    if (p->rec) lower_rec_pat(p, id);
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

/* Every pattern records its type, for the typed backend. */
static Type *infer_pat(Pat *p, Env **env, Env *stop) { return p->ty = infer_pat0(p, env, stop); }

static void check(Expr *e, Env *env, Type *expected);

static void infer_rhs(Rhs *r, Env *env, Type *res) {
  for (int i = 0; i < r->nwg; i++) infer_group(&r->wg[i], &env, false);
  for (int i = 0; i < r->ng; i++) {
    if (r->guards[i]) check(r->guards[i], env, tcon0(s_bool));
    check(r->bodies[i], env, res);
  }
}

static Type *infer_case(Expr *e, Env *env, Type *res) {
  Type **ts = xmalloc((size_t)e->ns * sizeof *ts + 1);
  for (int i = 0; i < e->ns; i++) ts[i] = infer(e->scruts[i], env);
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

/* Infer e against a known type. The expected type is pushed into lambdas,
   `let`, `if` and `case` alternatives, so a mismatch is reported at the
   innermost expression that has the wrong type, with the expected type's
   information (a signature's parameter types) already in place. */
static void check(Expr *e, Env *env, Type *expected) {
  switch (e->k) {
  case E_LAM: {
    e->ty = expected;
    Type *t = expected;
    e->lam->ptypes = xmalloc((size_t)e->lam->n * sizeof(Type *) + 1);
    Type *last = NULL;
    for (int i = 0; i < e->lam->n; i++) {
      Type *f = repr(t), *p, *r;
      if (f->k == TY_CON && f->name == s_arrow) {
        p = f->args[0];
        r = f->args[1];
        last = f;
      } else {
        p = newvar();
        r = newvar();
        last = tarrow(p, r);
        unify(f, last, e->line, e->col);
      }
      env = env_push(env, e->lam->params[i], p);
      e->lam->ptypes[i] = p;
      t = r;
    }
    Type *saved = cur_eff;
    cur_eff = last && last->eff ? last->eff : newvar();
    for (int i = 0; i < e->lam->n; i++) VPUSH(param_stack, e->lam->ptypes[i]);
    check(e->lam->body, env, t);
    param_stack.n -= e->lam->n;
    cur_eff = saved;
    return;
  }
  case E_LET:
    e->ty = expected;
    infer_group(&e->g, &env, false);
    check(e->body, env, expected);
    return;
  case E_IF:
    e->ty = expected;
    check(e->cond, env, tcon0(s_bool));
    check(e->then_, env, expected);
    check(e->else_, env, expected);
    return;
  case E_CASE:
    e->ty = expected;
    infer_case(e, env, expected);
    return;
  case E_APP: { /* a constructor: result first, then each argument against its field */
    if (e->fn->k != E_CON) break;
    e->ty = expected;
    Type *t = infer(e->fn, env);
    Type **ps = xmalloc((size_t)e->n * sizeof *ps);
    for (int i = 0; i < e->n; i++) {
      Type *f = repr(t);
      if (f->k == TY_CON && f->name == s_arrow) {
        ps[i] = f->args[0];
        t = f->args[1];
      } else {
        ps[i] = newvar();
        Type *r = newvar();
        unify(f, tarrow(ps[i], r), e->line, e->col);
        t = r;
      }
    }
    unify(expected, t, e->line, e->col);
    for (int i = 0; i < e->n; i++) check(e->args[i], env, ps[i]);
    free(ps);
    return;
  }
  default: break;
  }
  unify(expected, infer(e, env), e->line, e->col);
}

static Type *infer0(Expr *e, Env *env);

/* The number of arguments that runs the body of fn, if fn is known; 0
   otherwise. A local is unknown: its binding may not be a lambda. */
static int known_arity(Expr *fn, Env *env) {
  if (fn->k == E_LAM) return fn->lam->n;
  if (fn->k != E_VAR) return 0;
  for (Env *x = env; x; x = x->next)
    if (x->name == fn->name) return 0;
  int g = map_get(&gmap, fn->name);
  if (g < 0) return 0;
  if (globals.a[g].prim >= 0) return fk_prims[globals.a[g].prim].arity;
  if (globals.a[g].ffi) return globals.a[g].ffi->arity;
  if (globals.a[g].mcls >= 0) return classes.a[globals.a[g].mcls].arity[globals.a[g].midx]; /* not the selector's */
  Bind *b = globals.a[g].bind;
  Expr *x = b ? b->orig ? b->orig : b->e : NULL; /* before elaboration added dictionary parameters */
  return x && x->k == E_LAM ? x->lam->n : 0;
}

/* Every expression records its type, for the typed backend. */
static Type *infer(Expr *e, Env *env) {
  Type *t = infer0(e, env);
  e->ty = t;
  return t;
}

static Type *infer0(Expr *e, Env *env) {
  switch (e->k) {
  case E_VAR: {
    for (Env *x = env; x; x = x->next)
      if (x->name == e->name) return inst_var(x->t, e);
    int g = map_get(&gmap, e->name);
    if (g < 0 && isupper((unsigned char)e->name[0])) return infer_qualified_field(e, env);
    if (g < 0) cerror(e->line, e->col, "variable not in scope: `%s`", e->name);
    return inst_var(globals.a[g].type, e);
  }
  case E_CON:
    e->idx = con_lookup(e->name, e->line, e->col);
    return inst(condefs.a[e->idx].scheme, false);
  case E_INT: return e->ty = numvar(e->line, e->col);
  case E_FLOAT: return tcon0(s_float);
  case E_CHAR: return tcon0(s_char);
  case E_STR: return tcon0(s_string);
  case E_APP: {
    Type *tf = infer(e->fn, env);
    int known = known_arity(e->fn, env);
    for (int i = 0; i < e->n; i++) {
      Type *ta = infer(e->args[i], env);
      Type *f = repr(tf), *arrow;
      if (f->k == TY_CON && f->name == s_arrow) {
        unify(f->args[0], ta, e->args[i]->line, e->args[i]->col);
        tf = f->args[1];
        arrow = f;
      } else {
        Type *r = newvar();
        arrow = tarrow(ta, r);
        unify(f, arrow, e->args[i]->line, e->args[i]->col);
        tf = r;
      }
      if (!known || i >= known - 1) {
        uline = e->line;
        ucol = e->col;
        if (arrow == f) call_effect(arrow->eff);
        else VPUSH(deferred, ((Deferred){arrow->eff, cur_eff, e->line, e->col, cur_bind}));
      }
    }
    return tf;
  }
  case E_LAM: {
    Lambda *L = e->lam;
    Type **ps = xmalloc((size_t)L->n * sizeof *ps + 1);
    for (int i = 0; i < L->n; i++) env = env_push(env, L->params[i], ps[i] = newvar());
    L->ptypes = ps;
    Type *saved = cur_eff, *body = newvar();
    cur_eff = body;
    for (int i = 0; i < L->n; i++) VPUSH(param_stack, ps[i]);
    Type *t = infer(L->body, env);
    param_stack.n -= L->n;
    cur_eff = saved;
    for (int i = L->n; i-- > 0;) {
      t = tarrow(ps[i], t);
      if (i == L->n - 1) t->eff = body; /* the arrow that runs the body */
    }
    return t;
  }
  case E_LET:
    infer_group(&e->g, &env, false);
    return infer(e->body, env);
  case E_IF: {
    check(e->cond, env, tcon0(s_bool));
    Type *t = infer(e->then_, env);
    check(e->else_, env, t);
    return t;
  }
  case E_CASE: return infer_case(e, env, newvar());
  case E_ANN: {
    Type *t = infer(e->body, env);
    unify(inst(sig_scheme(e->sig), false), t, e->line, e->col);
    return t;
  }
  case E_FIELD: {
    Pending pd = {e, infer(e->body, env), newvar(), NULL};
    VPUSH(pending, pd);
    return pd.res;
  }
  case E_RECORD: {
    if (e->fn->k == E_CON) {
      lower_construction(e);
      return infer(e, env);
    }
    Pending pd = {e, infer(e->fn, env), NULL, xmalloc((size_t)e->n * sizeof(Type *) + 1)};
    for (int i = 0; i < e->n; i++) pd.vals[i] = infer(e->args[i], env);
    VPUSH(pending, pd);
    return pd.rec;
  }
  }
  abort();
}

/* Does v occur as the effect of an arrow within t? */
static bool eff_in(Type *t, Type *v) {
  t = repr(t);
  if (t->k != TY_CON) return false;
  if (t->eff && repr(t->eff) == v) return true;
  for (int i = 0; i < t->n; i++)
    if (eff_in(t->args[i], v)) return true;
  return false;
}

/* Can a binding with effect e act? If e is effectful, or the effect of an
   argument of a function around it, which callers choose. */
static void effectful_binding(Bind *b, const char *in) {
  if (b->stmt) { /* a `let` statement in `do` runs on its line */
    b->acts = true;
    return;
  }
  uline = b->line;
  ucol = b->col;
  cerror(uline, ucol,
         "in `%s`: `%s` can act, so it cannot be a lazy `where` or `let ... in` binding; bind it in `do` (`%s <- ...`, "
         "or a `let` statement), or make it a function (`%s () = ...`)",
         in, b->name, b->name, b->name);
}

static bool acts_by(Type *e, Types *params) {
  e = repr(e);
  if (e == eff_eff) return true;
  if (e->k != TY_VAR) return false;
  for (int k = 0; k < params->n; k++)
    if (eff_in(params->a[k], e)) return true;
  return false;
}

static void infer_group(Group *g, Env **env, bool top) {
  Env *none = NULL;
  if (!env) env = &none;
  level++;
  int ndeferred = deferred.n; /* this group's calls of functions not yet known */
  int nlocal = local_effs.n;
  Type **mono = xmalloc((size_t)g->n * sizeof *mono);
  Type **sch = xmalloc((size_t)g->n * sizeof *sch);
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    sch[i] = b->scheme ? b->scheme : b->sig ? sig_scheme(b->sig) : NULL;
    mono[i] = sch[i] ? sch[i] : newvar();
    b->type = mono[i];
    if (top) globals.a[b->gidx].type = mono[i];
    else *env = env_push(*env, b->name, mono[i]);
  }
  Type **evals = xcalloc((size_t)g->n + 1, sizeof *evals); /* each binding's evaluation effect */
  const char *saved_bind = cur_bind;
  Type *outer = cur_eff;
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    cur_bind = top || !saved_bind ? b->name : saved_bind;
    cur_eff = evals[i] = newvar();
    if (sch[i]) {
      Subst s = {0};
      Type *rigid = inst_rec(sch[i], &s, true);
      b->skolem = s.a;
      b->nskolem = s.n / 2;
      check(b->e, *env, rigid);
    } else {
      check(b->e, *env, mono[i]);
    }
  }
  lower_known();
  default_receivers(g, mono, sch);
  resolve_fields(false);
  settle_deferred(ndeferred); /* before generalization: a merged effect must be in the scheme */
  for (int i = 0; i < g->n; i++) {
    Bind *b = g->b[i];
    bool value = b->e->k != E_LAM;
    uline = b->line;
    ucol = b->col;
    cur_bind = top || !saved_bind ? b->name : saved_bind;
    /* main's evaluation is the program, and $it is an expression typed at the REPL */
    if (top && value && repr(evals[i]) == eff_eff && strcmp(b->name, "main") != 0 && b->name[0] != '$')
      eff_error("a top-level value acts when evaluated; make it a function, or run it from `main`");
    if (!top && outer) { /* a local binding runs within the enclosing code */
      Type *e = repr(evals[i]);
      bool tied = e->k == TY_VAR && e->level < level; /* to something outside the binding */
      if (value && acts_by(e, &param_stack)) effectful_binding(b, cur_bind);
      else if (value && tied) {
        LocalEff l = {b, e, {0}, cur_bind};
        for (int k = 0; k < param_stack.n; k++) VPUSH(l.params, param_stack.a[k]);
        VPUSH(local_effs, l);
      }
      /* A free effect means only pure calls: merging it would let the
         enclosing function's effect make the binding look effectful. */
      if (!value || e->k != TY_VAR || tied || b->acts) {
        cur_eff = outer;
        call_effect(evals[i]);
      }
    }
  }
  cur_bind = saved_bind;
  cur_eff = outer;
  level--;
  /* Local bindings without a signature never generalize constrained
     variables: they take them from their uses, so specializing the
     enclosing function specializes them too. */
  for (int i = 0; i < g->n; i++)
    if (!sch[i]) gen(mono[i], !top || g->b[i]->e->k != E_LAM);
  /* Only variables generic now belong to this binding; an enclosing
     binding's generalization later must not add to them. */
  for (int i = 0; i < g->n; i++) g->b[i]->npreds = preds_of(mono[i], &g->b[i]->preds);
  if (top) {
    for (int i = nlocal; i < local_effs.n; i++)
      if (acts_by(local_effs.a[i].eff, &local_effs.a[i].params)) effectful_binding(local_effs.a[i].b, local_effs.a[i].in);
    local_effs.n = nlocal;
  }
  free(mono);
  free(sch);
}

/* A Num variable left open at the end of the chunk becomes Int; any other
   constrained variable is ambiguous. */
static void default_classes(void) {
  for (int i = 0; i < cvars.n; i++) {
    Type *r = repr(cvars.a[i]);
    if (r->k != TY_VAR || r->skolem || r->level == GENERIC || !r->cls) continue;
    uline = r->line;
    ucol = r->col;
    if (r->cls & num_bit) {
      Type *it = tcon0(s_int);
      entail(it, r->cls);
      r->link = it;
      continue;
    }
    uint64_t m = minimal(r->cls);
    int c = 0;
    while (!(m & BIT(c))) c++;
    cerror(r->line, r->col, "ambiguous type: cannot choose a type for `%s a`; add a type annotation", classes.a[c].name);
  }
  cvars.n = 0;
}

static bool type_eq(Type *a, Type *b) {
  a = repr(a);
  b = repr(b);
  if (a == b) return true;
  if (a->k != TY_CON || b->k != TY_CON || a->name != b->name || a->n != b->n) return false;
  for (int i = 0; i < a->n; i++)
    if (!type_eq(a->args[i], b->args[i])) return false;
  return true;
}

/* -------------------------------------------------------- exhaustiveness */

/* Maranget's usefulness algorithm, extended to return a witness: a row of
   patterns that no row of the matrix matches, or NULL if the rows are
   exhaustive. Int and Char literals are treated as infinite domains. */
static Pat wild_pat = {.k = P_WILD};

static bool is_wild(Pat *p) { return p->k == P_VAR || p->k == P_WILD; }

static Pat **missing(Pat ***rows, int nr, int n) {
  if (n == 0) return nr ? NULL : xmalloc(1);
  int td = -1;
  for (int r = 0; r < nr; r++)
    if (rows[r][0]->k == P_CON) td = condefs.a[rows[r][0]->con].tydef;
  int first = 0, nc = 0, unseen = -1;
  if (td >= 0) {
    while (condefs.a[first].tydef != td) first++;
    nc = tydefs.a[td].ncons;
    for (int c = first; c < first + nc && unseen < 0; c++) {
      unseen = c;
      for (int r = 0; r < nr; r++)
        if (rows[r][0]->k == P_CON && rows[r][0]->con == c) unseen = -1;
    }
  }
  Pat ***sub = xmalloc((size_t)nr * sizeof *sub + 1);
  if (td >= 0 && unseen < 0) { /* every constructor appears: split on each */
    for (int c = first; c < first + nc; c++) {
      int ar = condefs.a[c].arity, ns = 0;
      for (int r = 0; r < nr; r++) {
        Pat *h = rows[r][0];
        if (!is_wild(h) && h->con != c) continue;
        Pat **row = xmalloc((size_t)(ar + n - 1) * sizeof *row + 1);
        for (int k = 0; k < ar; k++) row[k] = is_wild(h) ? &wild_pat : h->args[k];
        copy_n(row + ar, rows[r] + 1, (size_t)(n - 1) * sizeof *row);
        sub[ns++] = row;
      }
      Pat **w = missing(sub, ns, ar + n - 1);
      if (!w) continue;
      Pat *p = NEW(Pat);
      p->k = P_CON;
      p->con = c;
      p->n = ar;
      p->args = w;
      Pat **out = xmalloc((size_t)n * sizeof *out);
      out[0] = p;
      copy_n(out + 1, w + ar, (size_t)(n - 1) * sizeof *out);
      return out;
    }
    return NULL;
  }
  int ns = 0; /* default matrix: rows that match any head */
  for (int r = 0; r < nr; r++)
    if (is_wild(rows[r][0])) sub[ns++] = rows[r] + 1;
  Pat **w = missing(sub, ns, n - 1);
  if (!w) return NULL;
  Pat *p = &wild_pat;
  if (unseen >= 0) {
    p = NEW(Pat);
    p->k = P_CON;
    p->con = unseen;
    p->n = condefs.a[unseen].arity;
    p->args = xmalloc((size_t)p->n * sizeof *p->args + 1);
    for (int k = 0; k < p->n; k++) p->args[k] = &wild_pat;
  }
  Pat **out = xmalloc((size_t)n * sizeof *out);
  out[0] = p;
  copy_n(out + 1, w, (size_t)(n - 1) * sizeof *out);
  return out;
}

static void pat_str(SB *b, Pat *p, bool atom) {
  if (p->k != P_CON) { sb_puts(b, "_"); return; }
  const char *name = condefs.a[p->con].name;
  if (!p->n) { sb_puts(b, name); return; }
  if (is_tuple_name(name)) {
    for (int i = 0; i < p->n; i++) {
      sb_puts(b, i ? ", " : "(");
      pat_str(b, p->args[i], false);
    }
    sb_puts(b, ")");
    return;
  }
  if (atom) sb_puts(b, "(");
  if (p->con == FK_C_CONS) {
    pat_str(b, p->args[0], true);
    sb_puts(b, ":");
    pat_str(b, p->args[1], false);
  } else {
    sb_puts(b, name);
    for (int i = 0; i < p->n; i++) {
      sb_puts(b, " ");
      pat_str(b, p->args[i], true);
    }
  }
  if (atom) sb_puts(b, ")");
}

static bool always(Expr *g) {
  return !g || (g->k == E_CON && g->idx == FK_C_TRUE) ||
         (g->k == E_VAR && g->vk == VK_GLOBAL && globals.a[g->idx].name == s_otherwise);
}

static int warn_line, warn_col; /* pattern bindings share one case location */

/* An alternative covers its patterns only if one of its guards cannot fail. */
static void check_case(Expr *e, const char *fn) {
  VEC(Pat **) rows = {0};
  for (int a = 0; a < e->na; a++) {
    Rhs *r = &e->alts[a]->rhs;
    bool total = false;
    for (int i = 0; i < r->ng; i++) total |= always(r->guards[i]);
    if (total) VPUSH(rows, e->alts[a]->pats);
  }
  Pat **w = missing(rows.a, rows.n, e->ns);
  free(rows.a);
  if (!w || (e->line == warn_line && e->col == warn_col)) return;
  warn_line = e->line;
  warn_col = e->col;
  const char *in = fn && fn[0] != '$' ? xsprintf(" in `%s`", fn) : "";
  if (!e->ns) {
    cwarning(e->line, e->col, "guards%s may all fail", in);
    return;
  }
  SB b = {0};
  for (int i = 0; i < e->ns; i++) {
    if (i) sb_puts(&b, " ");
    pat_str(&b, w[i], e->ns > 1);
  }
  cwarning(e->line, e->col, "non-exhaustive patterns%s; not matched: %s", in, b.s);
}

bool case_covers(Expr *e, bool otherwise_true) {
  VEC(Pat **) rows = {0};
  for (int a = 0; a < e->na; a++) {
    Rhs *r = &e->alts[a]->rhs;
    bool total = false;
    for (int i = 0; i < r->ng; i++) {
      Expr *g = r->guards[i];
      total |= !g || (g->k == E_CON && g->idx == FK_C_TRUE) || (otherwise_true && g->k == E_VAR && g->name == s_otherwise);
    }
    if (total) VPUSH(rows, e->alts[a]->pats);
  }
  Pat **w = missing(rows.a, rows.n, e->ns);
  free(rows.a);
  return !w;
}

static void cover_expr(Expr *e, const char *fn);

static void cover_group(Group *g) {
  for (int i = 0; i < g->n; i++) cover_expr(g->b[i]->e, g->b[i]->name);
}

static void cover_expr(Expr *e, const char *fn) {
  switch (e->k) {
  case E_APP:
    cover_expr(e->fn, fn);
    for (int i = 0; i < e->n; i++) cover_expr(e->args[i], fn);
    return;
  case E_LAM: cover_expr(e->lam->body, e->lam->name ? e->lam->name : fn); return;
  case E_LET:
    cover_group(&e->g);
    cover_expr(e->body, fn);
    return;
  case E_IF:
    cover_expr(e->cond, fn);
    cover_expr(e->then_, fn);
    cover_expr(e->else_, fn);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) cover_expr(e->scruts[i], fn);
    check_case(e, fn);
    for (int a = 0; a < e->na; a++) {
      Rhs *r = &e->alts[a]->rhs;
      for (int i = 0; i < r->nwg; i++) cover_group(&r->wg[i]);
      for (int i = 0; i < r->ng; i++) {
        if (r->guards[i]) cover_expr(r->guards[i], fn);
        cover_expr(r->bodies[i], fn);
      }
    }
    return;
  case E_ANN: cover_expr(e->body, fn); return;
  default: return;
  }
}

/* -------------------------------------------------------------- classes */

/* Prelude overloads that elab.c replaces by primitives at Int, Float and
   Char: `x == y` at Int calls primEqInt. */
static const char *const spec_table[][2] = {
    {"==", "Eq"}, {"/=", "Ne"}, {"<", "Lt"}, {"<=", "Le"}, {">", "Gt"}, {">=", "Ge"}, {"+", "Add"},
    {"-", "Sub"}, {"*", "Mul"}, {"negate", "Negate"}, {"abs", "Abs"}, {"show", "Show"}, {"print", "Print"},
    {"++", "Append"}, {"length", "Length"}};

static const char *spec_stem(const char *name) {
  if (strcmp(cur_file, "<prelude>") != 0) return NULL;
  for (size_t i = 0; i < sizeof spec_table / sizeof spec_table[0]; i++)
    if (strcmp(spec_table[i][0], name) == 0) return spec_table[i][1];
  return NULL;
}

static int arrows(Type *t) {
  int n = 0;
  for (t = repr(t); t->k == TY_CON && t->name == s_arrow; t = repr(t->args[1])) n++;
  return n;
}

static bool mentions(Type *t, Type *v) {
  t = repr(t);
  if (t == v) return true;
  for (int i = 0; t->k == TY_CON && i < t->n; i++)
    if (mentions(t->args[i], v)) return true;
  return false;
}

/* Copy of t with variable from replaced by to. */
static Type *subst_var(Type *t, Type *from, Type *to) {
  t = repr(t);
  if (t == from) return to;
  if (t->k == TY_VAR || !t->n) return t;
  Type **args = xmalloc((size_t)t->n * sizeof *args);
  for (int i = 0; i < t->n; i++) args[i] = subst_var(t->args[i], from, to);
  Type *r = tcon(t->name, t->n, args);
  r->eff = t->eff;
  return r;
}

static Expr *ex_lam(int n, const char **params, Expr *body) {
  Expr *e = ex_new(E_LAM, body->line, body->col);
  Lambda *L = NEW(Lambda);
  L->n = n;
  L->params = params;
  L->body = body;
  L->line = body->line;
  L->col = body->col;
  e->lam = L;
  return e;
}

static const char **fresh_params(const char *prefix, int n) {
  const char **ps = xmalloc((size_t)n * sizeof *ps + 1);
  for (int i = 0; i < n; i++) ps[i] = intern(xsprintf("$%s%d", prefix, i));
  return ps;
}

/* \$d $x.. -> case $d of $D.C $s.. -> $s<field> $x.. */
static Bind *selector(const char *name, int con, int field, int arity, int line, int col) {
  const char **ps = fresh_params("x", arity + 1);
  ps[0] = intern("$d");
  Pat *p = con_pat(con, line, col);
  const char *s = intern("$s");
  p->args[field] = var_pat(s, line, col);
  Expr *body = ex_var(s, line, col);
  if (arity) {
    Expr **a = xmalloc((size_t)arity * sizeof *a);
    for (int i = 0; i < arity; i++) a[i] = ex_var(ps[i + 1], line, col);
    body = ex_app(body, arity, a);
  }
  Alt **alts = xmalloc(sizeof *alts);
  alts[0] = alt1(p, body);
  Expr *c = ex_new(E_CASE, line, col);
  make_case(c, ex_var(ps[0], line, col), 1, alts);
  Bind *b = NEW(Bind);
  *b = (Bind){.name = name, .e = ex_lam(arity + 1, ps, c), .line = line, .col = col};
  b->e->lam->name = name;
  return b;
}

static void add_global(const char *name, Type *t, Lambda *lam) {
  VPUSH(globals, ((Global){name, t, -1, lam, spec_stem(name), NULL, -1, -1, NULL}));
  map_put(&gmap, name, globals.n - 1);
}

int global_add(const char *name, Type *t, Lambda *lam) {
  add_global(name, t, lam);
  return globals.n - 1;
}

typedef VEC(Bind *) BindVec;

static void register_classes(Module *m, BindVec *extra, BindVec *raw) {
  for (int k = 0; k < m->ncl; k++) {
    ClassDecl *cd = m->classes[k];
    if (map_get(&clsmap, cd->name) >= 0) cerror(cd->line, cd->col, "class `%s` is already defined", cd->name);
    if (classes.n == 64) cerror(cd->line, cd->col, "too many classes (at most 64)");
    int id = classes.n;
    ClassDef c = {.name = cd->name, .closure = BIT(id), .nsup = cd->nsup, .sups = xmalloc((size_t)cd->nsup * sizeof(int) + 1)};
    for (int i = 0; i < cd->nsup; i++) {
      int s = map_get(&clsmap, cd->sups[i]);
      if (s < 0) cerror(cd->line, cd->col, "unknown class `%s`", cd->sups[i]);
      c.sups[i] = s;
      c.closure |= classes.a[s].closure;
    }
    c.var = gvar(c.closure);
    c.nmeth = cd->nsig;
    c.meths = cd->names;
    c.arity = xmalloc((size_t)c.nmeth * sizeof(int) + 1);
    c.hasdef = xcalloc((size_t)c.nmeth + 1, sizeof(bool));
    c.mtypes = xmalloc((size_t)c.nmeth * sizeof(Type *) + 1);
    for (int i = 0; i < c.nmeth; i++) {
      for (int j = 0; j < i; j++)
        if (c.meths[j] == c.meths[i]) cerror(cd->line, cd->col, "duplicate method `%s`", c.meths[i]);
      TVars tv = {0};
      VPUSH(tv.names, cd->var);
      VPUSH(tv.types, c.var);
      te_eff = gvar(0); /* as a signature: polymorphic at uses, rigid when an instance is checked */
      c.mtypes[i] = te_type(cd->sigs[i]->t, &tv, cd->sigs[i]);
      te_eff = NULL;
      c.arity[i] = arrows(c.mtypes[i]);
      if (!mentions(c.mtypes[i], c.var))
        cerror(cd->sigs[i]->line, cd->sigs[i]->col, "the type of method `%s` must mention `%s`", c.meths[i], cd->var);
      if (!c.arity[i])
        cerror(cd->sigs[i]->line, cd->sigs[i]->col, "method `%s` must be a function (constant methods are not supported)",
               c.meths[i]);
    }
    const char *dname = intern(xsprintf("$D.%s", cd->name));
    VPUSH(tydefs, ((TyDef){dname, 0, 1}));
    c.con = (int)fk_register_con(dname, c.nmeth + c.nsup);
    VPUSH(condefs, ((ConDef){dname, c.nmeth + c.nsup, NULL, tydefs.n - 1, NULL}));
    VPUSH(classes, c);
    map_put(&clsmap, cd->name, id);
    if (strcmp(cd->name, "Num") == 0) {
      num_bit = BIT(id);
      num_class = id;
    }
    ClassDef *cp = &classes.a[id];
    for (int i = 0; i < cp->nmeth; i++) {
      int prev = map_get(&gmap, cp->meths[i]);
      if (prev >= 0 && prev < nprelude)
        cerror(cd->sigs[i]->line, cd->sigs[i]->col, "method `%s` is defined in the prelude and cannot be redefined; choose another name",
               cp->meths[i]);
      Bind *s = selector(cp->meths[i], cp->con, i, cp->arity[i], cd->line, cd->col);
      VPUSH(*raw, s);
      add_global(cp->meths[i], cp->mtypes[i], s->e->lam);
      globals.a[globals.n - 1].mcls = id;
      globals.a[globals.n - 1].midx = i;
      if (num_class == id && strcmp(cp->meths[i], "fromInt") == 0) map_put(&gmap, intern("$fromInt"), globals.n - 1);
    }
    for (int i = 0; i < cp->nsup; i++) {
      const char *n = intern(xsprintf("$sup.%s.%s", cp->name, classes.a[cp->sups[i]].name));
      Bind *s = selector(n, cp->con, cp->nmeth + i, 0, cd->line, cd->col);
      VPUSH(*raw, s);
      add_global(n, NULL, s->e->lam);
    }
    for (int i = 0; i < cd->ndef; i++) {
      Bind *b = cd->defs[i];
      int mi = 0;
      while (mi < cp->nmeth && cp->meths[mi] != b->name) mi++;
      if (mi == cp->nmeth) cerror(b->line, b->col, "`%s` is not a method of class `%s`", b->name, cp->name);
      cp->hasdef[mi] = true;
      b->name = intern(xsprintf("$def.%s.%s", cp->name, cp->meths[mi]));
      b->scheme = cp->mtypes[mi];
      VPUSH(*extra, b);
    }
  }
}

/* An instance head `T a b`: the type constructor and its variables. */
static const char *head_tycon(InstDecl *in, Strs *vars) {
  TypeExpr *h = in->head;
  const char *tc;
  switch (h->k) {
  case TE_CON: tc = h->name; break;
  case TE_LIST: tc = s_list; break;
  case TE_TUPLE: {
    if (!h->n) { tc = s_unit; break; }
    char buf[16] = "(";
    for (int i = 1; i < h->n; i++) strcat(buf, ",");
    strcat(buf, ")");
    tc = intern(buf);
    break;
  }
  default: cerror(h->line, h->col, "instances for function types are not supported");
  }
  int ti = map_get(&tmap, tc);
  if (ti < 0) cerror(h->line, h->col, "unknown type `%s`", tc);
  if (tydefs.a[ti].arity != h->n)
    cerror(h->line, h->col, "type `%s` expects %d argument%s, got %d", tc, tydefs.a[ti].arity,
           tydefs.a[ti].arity == 1 ? "" : "s", h->n);
  for (int i = 0; i < h->n; i++) {
    TypeExpr *a = h->args[i];
    if (a->k != TE_VAR) cerror(a->line, a->col, "an instance head must be a type constructor applied to type variables");
    for (int j = 0; j < vars->n; j++)
      if (vars->a[j] == a->name) cerror(a->line, a->col, "type variable `%s` appears twice in the instance head", a->name);
    VPUSH(*vars, a->name);
  }
  return tc;
}

static void register_instances(Module *m, BindVec *extra, int *first) {
  *first = insts.n;
  for (int k = 0; k < m->ni; k++) {
    InstDecl *in = m->insts[k];
    int cls = map_get(&clsmap, in->cls);
    if (cls < 0) cerror(in->line, in->col, "unknown class `%s`", in->cls);
    Strs vars = {0};
    const char *tc = head_tycon(in, &vars);
    if (find_inst(cls, tc)) cerror(in->line, in->col, "duplicate instance `%s %s`", in->cls, tc);
    Type **vs = xmalloc((size_t)vars.n * sizeof *vs + 1);
    for (int i = 0; i < vars.n; i++) vs[i] = gvar(0);
    Sig *cx = in->ctx;
    for (int i = 0; i < cx->nctx; i++) {
      int v = 0;
      while (v < vars.n && vars.a[v] != cx->ctx[i]) v++;
      if (v == vars.n) cerror(in->line, in->col, "constraint on `%s`, which is not in the instance head", cx->ctx[i]);
      int c = map_get(&clsmap, cx->ctx_cls[i]);
      if (c < 0) cerror(in->line, in->col, "unknown class `%s`", cx->ctx_cls[i]);
      vs[v]->cls |= classes.a[c].closure;
    }
    const char *dname = intern(xsprintf("$dict.%s.%s", in->cls, tc));
    add_global(dname, NULL, NULL);
    VPUSH(insts, ((InstDef){cls, tc, vars.n, vs, globals.n - 1, in->line, in->col}));
    Type *head = tcon(tc, vars.n, vs);
    ClassDef *cp = &classes.a[cls];
    for (int i = 0; i < in->nb; i++) {
      int mi = 0;
      while (mi < cp->nmeth && cp->meths[mi] != in->binds[i]->name) mi++;
      if (mi == cp->nmeth)
        cerror(in->binds[i]->line, in->binds[i]->col, "`%s` is not a method of class `%s`", in->binds[i]->name, cp->name);
    }
    for (int mi = 0; mi < cp->nmeth; mi++) {
      Bind *b = NULL;
      for (int i = 0; i < in->nb; i++)
        if (in->binds[i]->name == cp->meths[mi]) b = in->binds[i];
      int ar = cp->arity[mi];
      const char **ps = fresh_params("x", ar);
      Expr **args = xmalloc((size_t)ar * sizeof *args);
      for (int i = 0; i < ar; i++) args[i] = ex_var(ps[i], in->line, in->col);
      if (!b) { /* \x.. -> $def.C.m x.. */
        if (!cp->hasdef[mi])
          cerror(in->line, in->col, "instance `%s %s` does not define `%s`, which has no default", in->cls, tc, cp->meths[mi]);
        b = NEW(Bind);
        Expr *f = ex_var(intern(xsprintf("$def.%s.%s", cp->name, cp->meths[mi])), in->line, in->col);
        *b = (Bind){.e = ex_lam(ar, ps, ex_app(f, ar, args)), .line = in->line, .col = in->col};
      } else if (b->e->k != E_LAM) { /* lambdas, so dictionaries can refer to them before values run */
        b->e = ex_lam(ar, ps, ex_app(b->e, ar, args));
      }
      b->name = intern(xsprintf("$m.%s.%s.%s", cp->name, tc, cp->meths[mi]));
      b->scheme = subst_var(cp->mtypes[mi], cp->var, head);
      VPUSH(*extra, b);
    }
  }
  for (int i = *first; i < insts.n; i++) {
    ClassDef *cp = &classes.a[insts.a[i].cls];
    for (int s = 0; s < cp->nsup; s++)
      if (!find_inst(cp->sups[s], insts.a[i].tycon))
        cerror(insts.a[i].line, insts.a[i].col, "instance `%s %s` needs an instance `%s %s`", cp->name,
               insts.a[i].tycon, classes.a[cp->sups[s]].name, insts.a[i].tycon);
  }
}

static int popcount(uint64_t m) {
  int n = 0;
  for (; m; m &= m - 1) n++;
  return n;
}

/* ---------------------------------------------------- type completeness */

/* With FK_CHECK_TYPES set, report every expression, pattern and lambda
   parameter left without a type. FK_CHECK_TYPES=2 also prints each
   top-level function's parameter and result types. The typed backend treats them as V, which
   is safe but slow, so a gap would otherwise go unnoticed. */
static int untyped;

static void untyped_at(const char *what, int line, int col) {
  fprintf(stderr, "%s:%d:%d: untyped %s\n", cur_file, line, col, what);
  untyped++;
}

static void ct_expr(Expr *e);

static void ct_pat(Pat *p) { /* a wildcard binds and tests nothing: no type needed */
  if (!p->ty && p->k != P_WILD) untyped_at("pattern", p->line, p->col);
  for (int i = 0; i < p->n; i++) ct_pat(p->args[i]);
}

static void ct_group(Group *g) {
  for (int i = 0; i < g->n; i++) {
    if (!g->b[i]->type) untyped_at("binding", g->b[i]->line, g->b[i]->col);
    ct_expr(g->b[i]->e);
  }
}

static void ct_expr(Expr *e) {
  if (!e->ty) untyped_at((const char *[]){"variable", "constructor", "integer", "float", "char", "string", "application",
                                          "lambda", "let", "if", "case", "annotation", "field", "record"}[e->k],
                         e->line, e->col);
  switch (e->k) {
  case E_APP:
    ct_expr(e->fn);
    for (int i = 0; i < e->n; i++) ct_expr(e->args[i]);
    return;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++)
      if (!e->lam->ptypes || !e->lam->ptypes[i]) untyped_at("parameter", e->line, e->col);
    ct_expr(e->lam->body);
    return;
  case E_LET:
    ct_group(&e->g);
    ct_expr(e->body);
    return;
  case E_IF:
    ct_expr(e->cond);
    ct_expr(e->then_);
    ct_expr(e->else_);
    return;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) ct_expr(e->scruts[i]);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      for (int i = 0; i < alt->np; i++) ct_pat(alt->pats[i]);
      for (int i = 0; i < alt->rhs.nwg; i++) ct_group(&alt->rhs.wg[i]);
      for (int i = 0; i < alt->rhs.ng; i++) {
        if (alt->rhs.guards[i]) ct_expr(alt->rhs.guards[i]);
        ct_expr(alt->rhs.bodies[i]);
      }
    }
    return;
  case E_ANN: ct_expr(e->body); return;
  default: return;
  }
}

void check_types(Bind **bs, int n) {
  const char *mode = getenv("FK_CHECK_TYPES");
  if (!mode) return;
  for (int i = 0; i < n; i++) {
    if (!bs[i]->type) untyped_at("binding", bs[i]->line, bs[i]->col);
    ct_expr(bs[i]->e);
    if (strcmp(mode, "2") || bs[i]->e->k != E_LAM) continue;
    Lambda *L = bs[i]->e->lam;
    fprintf(stderr, "%s:", bs[i]->name);
    for (int k = 0; k < L->n; k++) fprintf(stderr, " %s", L->ptypes[k] ? type_str(L->ptypes[k]) : "?");
    fprintf(stderr, " -> %s\n", L->body->ty ? type_str(L->body->ty) : "?");
  }
}

/* ---------------------------------------------------------------- driver */

static void add_tydef(const char *name, int arity, int ncons) {
  VPUSH(tydefs, ((TyDef){name, arity, ncons}));
  map_put(&tmap, name, tydefs.n - 1);
}

static void add_con(const char *name, Type *scheme, int arity, const char *ty) {
  con_effects(scheme);
  VPUSH(condefs, ((ConDef){name, arity, scheme, map_get(&tmap, ty), NULL}));
  map_put(&cmap, name, condefs.n - 1);
}

void compiler_init(void) {
  eff_pure = tcon0(intern("Pure"));
  eff_eff = tcon0(intern("Eff"));
  s_int = intern("Int");
  s_float = intern("Float");
  s_char = intern("Char");
  s_bool = intern("Bool");
  s_list = intern("[]");
  s_arrow = intern("->");
  s_unit = intern("()");
  s_string = intern("String");
  s_otherwise = intern("otherwise");
  static const char *tuples[] = {"(,)", "(,,)", "(,,,)", "(,,,,)", "(,,,,,)", "(,,,,,,)"};
  add_tydef(s_int, 0, 0);
  add_tydef(s_float, 0, 0);
  add_tydef(s_char, 0, 0);
  add_tydef(s_bool, 0, 2);
  add_tydef(s_unit, 0, 1);
  add_tydef(s_list, 1, 2);
  add_tydef(s_arrow, 2, 0);
  add_tydef(intern("Num"), 1, 0); /* reserved */
  add_tydef(s_string, 0, 0);       /* packed; index < NBUILTIN_TYPES marks builtins */
  for (int i = 0; i < FK_MAXTUPLE - 1; i++) add_tydef(intern(tuples[i]), i + 2, 1);
  add_tydef(intern("Ptr"), 1, 0); /* a C address, for foreign imports (ffi.md) */
  add_tydef(intern("FunPtr"), 1, 0);
  add_tydef(intern("StablePtr"), 1, 0);
  add_tydef(intern("Array"), 1, 0); /* C memory: realtime.md */
  add_tydef(intern("Ring"), 1, 0);
  add_tydef(intern("Field"), 2, 0); /* Field s a: the offset of an a in a foreign struct s */
  for (int i = 0; i < ffi_ntypes; i++) /* CInt and the other C types */
    if (map_get(&tmap, intern(ffi_types[i].name)) < 0) add_tydef(intern(ffi_types[i].name), 0, 0);

  fk_init();
  Type *bool_t = tcon0(s_bool);
  add_con(intern("False"), bool_t, 0, s_bool);
  add_con(intern("True"), bool_t, 0, s_bool);
  add_con(s_unit, tcon0(s_unit), 0, s_unit);
  Type *a = gvar(0);
  add_con(s_list, tlist(a), 0, s_list);
  add_con(intern(":"), tarrow(a, tarrow(tlist(a), tlist(a))), 2, s_list);
  for (int i = 0; i < FK_MAXTUPLE - 1; i++) {
    int n = i + 2;
    Type *vs[FK_MAXTUPLE];
    for (int j = 0; j < n; j++) vs[j] = gvar(0);
    Type *t = tcon(intern(tuples[i]), n, vs);
    for (int j = n; j-- > 0;) t = tarrow(vs[j], t);
    add_con(intern(tuples[i]), t, n, intern(tuples[i]));
  }
  for (int i = 0; i < fk_nprims; i++) {
    Global g = {intern(fk_prims[i].name), sig_scheme(parse_sig_text(prim_info[i].type)), i, NULL, NULL, NULL, -1, -1, NULL};
    const char *pn = g.name;
    if ((!strcmp(pn, "putStr") || !strcmp(pn, "putStrLn") || !strcmp(pn, "getLine") || !strncmp(pn, "primPrint", 9))) {
      Type *t = g.type;
      while (repr(repr(t)->args[1])->k == TY_CON && repr(repr(t)->args[1])->name == s_arrow) t = repr(t)->args[1];
      repr(t)->eff = eff_eff; /* the arrow that acts */
    }
    VPUSH(globals, g);
    map_put(&gmap, g.name, globals.n - 1);
  }
  dict_type = tcon0(intern("$Dict"));
  map_put(&gmap, intern("$error"), global_lookup(intern("error")));
}

static bool is_c_name(const char *s) {
  if (!isalpha((unsigned char)*s) && *s != '_') return false;
  for (; *s; s++)
    if (!isalnum((unsigned char)*s) && *s != '_') return false;
  return true;
}

/* "callback" and "queue" imports: FunPtr (Ptr c -> a1 -> ... -> r), where
   Ptr c is the context C passes back, a StablePtr to the funk closure. */
static void check_callback(ForeignDecl *f, Type *t) {
  const char *what = f->cb == 'c' ? "callback" : "queue";
  t = repr(t);
  if (t->k != TY_CON || strcmp(t->name, "FunPtr") || repr(repr(t->args[0]))->name != s_arrow)
    cerror(f->line, f->col, "a \"%s\" import has a type FunPtr (Ptr () -> ... -> r), the C function C will call", what);
  Type *a = repr(t->args[0]);
  if (ffi_kind(a->args[0]) != 'p')
    cerror(f->line, f->col, "foreign import `%s`: the first argument of a callback is its context, a `Ptr ()`, which C passes back", f->name);
  for (a = repr(a->args[1]); a->k == TY_CON && a->name == s_arrow; a = repr(a->args[1])) {
    int k = ffi_kind(a->args[0]);
    if (!k || k == 'v')
      cerror(f->line, f->col, "foreign import `%s`: a callback argument of type `%s` cannot come from C", f->name, type_str(a->args[0]));
  }
  int k = ffi_kind(a);
  if (f->cb == 'q' && k != 'v')
    cerror(f->line, f->col, "foreign import `%s`: a queued callback returns `()`, since C does not wait for it", f->name);
  if (!k || k == 's' || k == 'L')
    cerror(f->line, f->col, "foreign import `%s`: a callback cannot return `%s` to C", f->name, type_str(a));
}

/* Foreign imports become globals with their declared types (ffi.md). `->`
   is the importer's promise of purity, as a primitive's is. */
static void register_foreigns(Module *m) {
  for (int i = 0; i < m->nforeign; i++) {
    ForeignDecl *f = m->foreigns[i];
    if (f->exported) continue; /* check_exports, once the function's type is known */
    int prev = map_get(&gmap, f->name);
    if (prev >= 0 && prev < nprelude)
      cerror(f->line, f->col, "`%s` is defined in the prelude and cannot be redefined; choose another name", f->name);
    for (int j = 0; j < i; j++)
      if (m->foreigns[j]->name == f->name) cerror(f->line, f->col, "duplicate foreign import `%s`", f->name);
    for (int j = 0; j < m->nb; j++)
      if (m->binds[j]->name == f->name) cerror(m->binds[j]->line, m->binds[j]->col, "`%s` is already a foreign import", f->name);
    Type *t = sig_scheme(f->sig);
    f->pure = true;
    f->arity = 0;
    if (f->cb) {
      check_callback(f, t);
      f->gidx = globals.n;
      add_global(f->name, t, NULL);
      globals.a[f->gidx].ffi = f;
      continue;
    }
    if (f->arity == 0 && repr(t)->name == s_arrow && !is_c_name(f->cname))
      cerror(f->line, f->col, "foreign import `%s`: `%s` is not a C function name", f->name, f->cname);
    for (Type *a = repr(t); a->k == TY_CON && a->name == s_arrow; a = repr(a->args[1])) {
      if (ffi_kind(a->args[0]) == 'L')
        cerror(f->line, f->col, "foreign import `%s`: CStringLen is a pair of C parameters in a callback's type only; pass a String", f->name);
      if (!ffi_param_type(a->args[0]))
        cerror(f->line, f->col, "foreign import `%s`: an argument of type `%s` cannot be passed to C; use Int, Float, Char, Bool, String, Ptr or a C type such as CInt",
               f->name, type_str(a->args[0]));
      if (repr(a->args[0])->name == s_unit && (f->arity || repr(repr(a->args[1]))->name == s_arrow))
        cerror(f->line, f->col, "foreign import `%s`: `()` can only be the one argument, of a C function without parameters", f->name);
      f->pure &= repr(a->eff) != eff_eff;
      f->arity++;
    }
    Type *r = repr(t);
    for (int k = 0; k < f->arity; k++) r = repr(r->args[1]);
    if (!f->arity && r->name == s_unit) cerror(f->line, f->col, "foreign import `%s`: a C value cannot have type `()`", f->name);
    if (!ffi_param_type(r) || ffi_kind(r) == 'L')
      cerror(f->line, f->col, "foreign import `%s`: a result of type `%s` cannot come from C; use Int, Float, Char, Bool, (), String, Ptr or a C type such as CInt",
             f->name, type_str(r));
    f->gidx = globals.n;
    add_global(f->name, t, NULL);
    globals.a[f->gidx].ffi = f;
  }
}

/* foreign struct (realtime.md): an opaque type T, a constant T.f :: Field T a
   per field, its offset, and T.size :: Int. They are C values, computed by
   the glue with offsetof and sizeof, so C lays the struct out: the header's
   struct, whose field types the glue checks, or one the glue defines. */
typedef struct { const char *name, *ctype, *cdef; Strs headers; } CStruct; /* headers: its own and its nested structs' */
static VEC(CStruct) cstructs; /* foreign structs so far, for fields of their types */

static CStruct *cstruct(Type *t) {
  t = repr(t);
  for (int i = 0; t->k == TY_CON && !t->n && i < cstructs.n; i++)
    if (cstructs.a[i].name == t->name) return &cstructs.a[i];
  return NULL;
}

/* Struct types are named before data types, whose constructors may hold
   them (`Ptr S`); their fields, which may be data types, come later. */
static void register_struct_names(Module *m) {
  for (int i = 0; i < m->nstructs; i++) {
    StructDecl *s = m->structs[i];
    if (map_get(&tmap, s->name) >= 0) cerror(s->line, s->col, "type `%s` is already defined", s->name);
    VPUSH(tydefs, ((TyDef){s->name, 0, 0}));
    map_put(&tmap, s->name, tydefs.n - 1);
  }
}

static void register_structs(Module *m) {
  for (int i = 0; i < m->nstructs; i++) {
    StructDecl *s = m->structs[i];
    const char *ct = s->ctype ? s->ctype : xsprintf("struct fk_struct_%s", s->name);
    SB def = {0}, inner = {0}; /* inner: the definitions of structs this one holds, before it */
    Strs headers = {0};
    if (s->header) VPUSH(headers, s->header);
    VEC(ForeignDecl *) made = {0}; /* each carries the definition: the glue holds only what the program uses */
    sb_printf(&def, "#ifndef FK_STRUCT_%s\n#define FK_STRUCT_%s\n", s->name, s->name); /* once, though several structs include it */
    if (!s->ctype) sb_printf(&def, "%s {", ct);
    Type *st = tcon0(s->name);
    for (int k = 0; k < s->nf; k++) {
      StructField *f = &s->fields[k];
      for (int j = 0; j < k; j++)
        if (s->fields[j].name == f->name) cerror(s->line, s->col, "struct `%s`: duplicate field `%s`", s->name, f->name);
      TVars tv = {0};
      Type *ft = te_type(f->type, &tv, NULL);
      int kind = ffi_kind(ft);
      CStruct *nested = cstruct(ft); /* a struct by value: reached with fieldPtr */
      if (!nested && (!kind || kind == 'v' || kind == 's' || kind == 'L'))
        cerror(s->line, s->col, "struct `%s`: field `%s` has type `%s`; a field is a C scalar, a pointer or a foreign struct declared before it",
               s->name, f->name, type_str(ft));
      const char *path = f->cpath ? f->cpath : f->name, *c = nested ? nested->ctype : ffi_param_type(ft);
      if (nested) {
        sb_puts(&inner, nested->cdef);
        for (int h = 0; h < nested->headers.n; h++) VPUSH(headers, nested->headers.a[h]);
      }
      if (!s->ctype) sb_printf(&def, " %s %s;", c, f->name);
      else if (nested)
        sb_printf(&def, "_Static_assert(__builtin_types_compatible_p(__typeof__(((%s *)0)->%s), %s), \"struct %s: the C field %s is not %s\");\n", ct, path, c,
                  s->name, path, c);
      else if (kind == 'p') /* any C pointer: the field's own pointer type is the header's */
        sb_printf(&def, "_Static_assert(__builtin_classify_type(((%s *)0)->%s) == 5 && sizeof(((%s *)0)->%s) == sizeof(void *), "
                        "\"struct %s: the C field %s is not a pointer\");\n", ct, path, ct, path, s->name, path);
      else /* the C type, or an enumeration of its size */
        sb_printf(&def, "_Static_assert(__builtin_types_compatible_p(__typeof__(((%s *)0)->%s), %s) || (__builtin_classify_type(((%s *)0)->%s) == 3 && "
                        "sizeof(((%s *)0)->%s) == sizeof(%s)), \"struct %s: the C field %s is not %s\");\n",
                  ct, path, c, ct, path, ct, path, c, s->name, path, c);
      Type *args[2] = {st, ft};
      ForeignDecl *d = NEW(ForeignDecl);
      *d = (ForeignDecl){.name = intern(xsprintf("%s.%s", s->name, f->name)), .header = s->header,
                         .cname = intern(xsprintf("(int64_t)offsetof(%s, %s)", ct, path)), .line = s->line, .col = s->col, .pure = true};
      d->gidx = globals.n;
      add_global(d->name, tcon(intern("Field"), 2, args), NULL);
      globals.a[d->gidx].ffi = d;
      VPUSH(made, d);
    }
    if (!s->ctype) sb_puts(&def, " };\n");
    sb_puts(&def, "#endif\n");
    const char *all = xsprintf("%s%s", inner.n ? inner.s : "", def.s);
    VPUSH(cstructs, ((CStruct){s->name, ct, all, headers}));
    ForeignDecl *z = NEW(ForeignDecl);
    *z = (ForeignDecl){.name = intern(xsprintf("%s.size", s->name)), .header = s->header, .cname = intern(xsprintf("(int64_t)sizeof(%s)", ct)),
                       .line = s->line, .col = s->col, .pure = true};
    z->gidx = globals.n;
    add_global(z->name, tcon0(s_int), NULL);
    globals.a[z->gidx].ffi = z;
    VPUSH(made, z);
    for (int k = 0; k < made.n; k++) {
      made.a[k]->cdef = all;
      made.a[k]->xheaders = headers;
    }
  }
}

/* foreign export ccall "cname" name :: type (realtime.md, R4): name is a
   function of this chunk with that type, whose arguments and result are C
   types. Its compiled code is checked allocation-free after resolve. */
static void check_export_types(Module *m, int ng0) {
  for (int i = 0; i < m->nforeign; i++) {
    ForeignDecl *f = m->foreigns[i];
    if (!f->exported) continue;
    if (!is_c_name(f->cname)) cerror(f->line, f->col, "foreign export `%s`: `%s` is not a C function name", f->name, f->cname);
    int g = map_get(&gmap, f->name);
    if (g < ng0 || !globals.a[g].bind || globals.a[g].bind->e->k != E_LAM)
      cerror(f->line, f->col, "foreign export `%s`: export a function defined in this program", f->name);
    Type *t = sig_scheme(f->sig), *a = repr(t);
    int n = 0;
    for (; a->k == TY_CON && a->name == s_arrow; a = repr(a->args[1]), n++) {
      int k = ffi_kind(a->args[0]);
      if (!k || k == 's' || k == 'L' || (k == 'v' && (n || repr(repr(a->args[1]))->name == s_arrow)))
        cerror(f->line, f->col, "foreign export `%s`: an argument of type `%s` is not a C scalar or pointer", f->name, type_str(a->args[0]));
    }
    int k = ffi_kind(a);
    if (!n) cerror(f->line, f->col, "foreign export `%s` must be a function", f->name);
    if (!k || k == 's' || k == 'L') cerror(f->line, f->col, "foreign export `%s`: a result of type `%s` is not a C scalar or pointer", f->name, type_str(a));
    unify(t, inst(globals.a[g].type, false), f->line, f->col);
    f->xtype = t;
    f->gidx = g;
    f->arity = n;
  }
}

static void check_export_code(Module *m) {
  for (int i = 0; i < m->nforeign; i++) {
    ForeignDecl *f = m->foreigns[i];
    if (!f->exported) continue;
    const char *why = cgen_alloc_free(globals.a[f->gidx].lam);
    if (why)
      cerror(f->line, f->col, "foreign export `%s` is not safe to call from a foreign thread: %s", f->name, why);
    VPUSH(ffi_exports, f);
  }
}

bool compile_chunk(Module *m, Chunk *out) {
  jmp_buf jb, *saved = cerr_jmp;
  int mark = undo_mark(), ng0 = globals.n, nc0 = condefs.n, nt0 = tydefs.n, ncl0 = classes.n, ni0 = insts.n, ns0 = cstructs.n;
  uint64_t nb0 = num_bit;
  int nk0 = num_class;
  cvars.n = 0;
  level = 0;
  if (setjmp(jb)) {
    cerr_jmp = saved;
    undo_to(mark);
    globals.n = ng0;
    condefs.n = nc0;
    tydefs.n = nt0;
    classes.n = ncl0;
    insts.n = ni0;
    cstructs.n = ns0;
    num_bit = nb0;
    num_class = nk0;
    fk_truncate_cons((uint32_t)nc0);
    cvars.n = 0;
    pending.n = 0;
    level = 0;
    return false;
  }
  cerr_jmp = &jb;
  pending.n = 0;
  register_struct_names(m);
  register_datas(m);
  BindVec extra = {0}, raw = {0};
  int first_inst;
  register_classes(m, &extra, &raw);
  register_instances(m, &extra, &first_inst);
  register_structs(m);
  register_foreigns(m);
  for (int i = 0; i < m->nb; i++) VPUSH(extra, m->binds[i]);
  m->binds = extra.a; /* generated bindings first; order within a group follows */
  m->nb = extra.n;
  for (int i = 0; i < m->nb; i++) {
    Bind *b = m->binds[i];
    const char *dot = strchr(b->name, '.');
    if (isupper((unsigned char)b->name[0]) && dot) { /* Type.name */
      const char *tn = intern_n(b->name, (size_t)(dot - b->name));
      int ti = map_get(&tmap, tn);
      if (ti < 0) cerror(b->line, b->col, "unknown type `%s`", tn);
      if (tydefs.a[ti].ncons && has_label(ti, intern(dot + 1)))
        cerror(b->line, b->col, "type `%s` already has a field `%s`", tn, dot + 1);
    }
    int prev = map_get(&gmap, b->name);
    if (prev >= 0 && prev < nprelude && b->name[0] != '$') /* the prelude's names are reserved */
      cerror(b->line, b->col, "`%s` is defined in the prelude and cannot be redefined; choose another name, or a type namespace (`T.%s`)",
             b->name, b->name);
    if (b->e->k == E_LAM) b->e->lam->name = b->name;
    b->gidx = globals.n;
    add_global(b->name, NULL, b->e->k == E_LAM ? b->e->lam : NULL);
    globals.a[b->gidx].bind = b;
  }
  Group tmp = {m->binds, m->nb, false};
  split_group_exprs(&tmp);
  int ng;
  Group *gs = scc(m->binds, m->nb, &ng);
  for (int i = 0; i < ng; i++) infer_group(&gs[i], NULL, true);
  default_classes();
  check_export_types(m, ng0);
  /* Evaluate `main` last, unless another binding uses it. */
  for (int i = 0; i < ng; i++) {
    if (gs[i].n != 1 || strcmp(gs[i].b[0]->name, "main") != 0) continue;
    Deps d = {gs[i].b, 1, 0, (bool[1]){false}};
    for (int j = 0; j < m->nb; j++)
      if (m->binds[j] != gs[i].b[0]) deps_expr(m->binds[j]->e, NULL, &d);
    if (!d.adj[0]) {
      Group g = gs[i];
      move_n(&gs[i], &gs[i + 1], (size_t)(ng - i - 1) * sizeof *gs);
      gs[ng - 1] = g;
    }
    break;
  }
  for (int i = 0; i < m->nb; i++)
    if (m->binds[i]->npreds && m->binds[i]->e->k == E_LAM) m->binds[i]->orig = copy_expr(m->binds[i]->e);
  specialized.n = 0;
  for (int i = 0; i < m->nb; i++) elab_bind(m->binds[i]);
  check_types(m->binds, m->nb);
  check_types(specialized.a, specialized.n);
  for (int i = 0; i < specialized.n; i++) VPUSH(raw, specialized.a[i]);
  int nspec = specialized.n;
  lift_locals(m->binds, m->nb);
  lift_locals(specialized.a, nspec); /* type-class clones made above */
  VEC(Bind *) cand = {0};
  for (int i = 0; i < m->nb; i++) VPUSH(cand, m->binds[i]);
  for (int i = 0; i < specialized.n; i++) VPUSH(cand, specialized.a[i]);
  lift_locals(cand.a, fuse_lists(cand.a, cand.n)); /* the fused loops */
  ho_specialize(m->binds, m->nb);
  ho_specialize(specialized.a, specialized.n); /* clones and lifted functions */
  reuse_cells(m->binds, m->nb, specialized.a, specialized.n);
  for (int i = nspec; i < specialized.n; i++) VPUSH(raw, specialized.a[i]);
  /* Dictionaries after the selectors, superclass dictionaries first. */
  for (int depth = 1; depth <= 64; depth++)
    for (int i = first_inst; i < insts.n; i++)
      if (popcount(classes.a[insts.a[i].cls].closure) == depth) VPUSH(raw, dict_bind(&insts.a[i]));
  for (int i = 0; i < m->nb; i++) resolve_top(m->binds[i]);
  for (int i = 0; i < raw.n; i++) {
    Bind *b = raw.a[i];
    b->gidx = global_lookup(b->name);
    if (b->e->k == E_LAM) globals.a[b->gidx].lam = b->e->lam;
    resolve_top(b);
  }
  check_export_code(m);
  warn_line = warn_col = 0;
  for (int i = 0; i < m->nb; i++) cover_expr(m->binds[i]->e, m->binds[i]->name);
  if (!strcmp(cur_file, "<prelude>")) {
    nprelude = globals.n;
    nprelude_types = tydefs.n;
    nprelude_cons = condefs.n;
  }
  out->groups = gs;
  out->ng = ng;
  out->raw = raw.a;
  out->nraw = raw.n;
  for (int i = 0; i < m->nlibs; i++) {
    bool seen = false;
    for (int j = 0; j < ffi_libs.n; j++) seen |= !strcmp(ffi_libs.a[j], m->libs[i]);
    if (!seen) VPUSH(ffi_libs, m->libs[i]);
  }
  cerr_jmp = saved;
  return true;
}

const char *infer_type_text(Expr *e) {
  jmp_buf jb, *saved = cerr_jmp;
  if (setjmp(jb)) {
    cerr_jmp = saved;
    cvars.n = 0;
    pending.n = 0;
    level = 0;
    return NULL;
  }
  cerr_jmp = &jb;
  cvars.n = 0;
  pending.n = 0;
  split_expr(e);
  level = 1;
  Type *t = infer(e, NULL);
  resolve_fields(true);
  level = 0;
  gen(t, false);
  cvars.n = 0;
  cerr_jmp = saved;
  return type_str(t);
}
