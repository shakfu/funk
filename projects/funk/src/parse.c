/* parse.c -- recursive-descent parser. Desugars as it goes:
     multi-clause functions   -> \fresh.. -> case fresh.. of clauses
     x op y                   -> op x y   (&& and || become `if`)
     (op e), (e op)           -> flip op e, op e
     [a, b], [a..b], "str"    -> cons chains, enumFromTo
     (p1, p2) = e             -> one binding per pattern variable
     do { p <- e; e2; let ds; e3 }
                              -> case e of p -> case e2 of _ -> let ds in e3
   Record syntax (p.x, C { f = e }, r { f = e }) is kept as E_FIELD and
   E_RECORD; check.c lowers it once constructor fields are known. */
#include <ctype.h>

#include "funk.h"

typedef struct { Token *t; int n, pos; } P;

enum { D_CLAUSE, D_PATBIND, D_SIG, D_DATA, D_FIXITY, D_CLASS, D_INST, D_FOREIGN, D_LIBRARY, D_STRUCT };
typedef struct {
  int k, line, col;
  const char *name;
  int np;
  Pat **pats;
  Rhs rhs;
  Pat *pat;
  const char **names; /* D_SIG, D_FIXITY */
  int nn;
  Sig *sig;
  DataDecl *data;
  ClassDecl *cls;
  InstDecl *inst;
  ForeignDecl *ffi; /* D_FOREIGN */
  StructDecl *st;   /* D_STRUCT */
  const char *lib;  /* D_LIBRARY */
} Decl;
typedef VEC(Decl) Decls;
typedef VEC(Bind *) Binds;
typedef VEC(Pat *) Pats;
typedef VEC(DataDecl *) Datas;

static Expr *parse_expr(P *p);
static Pat *parse_pat(P *p);
static Pat *parse_apat(P *p);
static TypeExpr *parse_type(P *p);
static void parse_decl_block(P *p, Binds *out);
static void parse_decl(P *p, Decls *out, bool top);
static void build_binds(Decls *ds, Binds *out, Datas *datas);

static Token *cur(P *p) { return &p->t[p->pos]; }
static Token *ahead(P *p, int k) { return &p->t[p->pos + k < p->n ? p->pos + k : p->n - 1]; }
static bool is(P *p, TokKind k) { return cur(p)->k == k; }
static Token *next(P *p) {
  Token *t = cur(p);
  if (p->pos < p->n - 1) p->pos++;
  return t;
}
static bool accept(P *p, TokKind k) {
  if (!is(p, k)) return false;
  next(p);
  return true;
}

static _Noreturn void unexpected(P *p, const char *what) {
  Token *t = cur(p);
  const char *got = tok_name(t->k);
  if (t->k == T_VARID || t->k == T_CONID || t->k == T_VARSYM || t->k == T_CONSYM)
    got = xsprintf("'%s'", t->s);
  cerror(t->line, t->col, "expected %s, found %s", what, got);
}

static Token *expect(P *p, TokKind k, const char *what) {
  if (!is(p, k)) unexpected(p, what ? what : tok_name(k));
  return next(p);
}

/* parse.c's state, one per session (embedding.md, phase 2) */
typedef struct {
  int fresh_counter;
  /* Declared fixities, as prec * 3 + index in "lrn". Kept across inputs so
     that later REPL inputs see them; main.c rolls them back with a failed input. */
  Map user_fixities;
} ParseState;
static const ParseState parse_init = {0};
#define ST ((ParseState *)fk_here()->cc[CC_PARSE])

void *parse_state_new(void) {
  ParseState *s = malloc(sizeof *s);
  if (!s) fk_fatal("out of memory");
  *s = parse_init;
  return s;
}

static const char *fresh(const char *prefix) { return intern(xsprintf("$%s%d", prefix, ST->fresh_counter++)); }

/* Type.name: a constructor token directly followed by a field token. */
static bool qual_start(P *p, int off) { return ahead(p, off)->k == T_CONID && ahead(p, off + 1)->k == T_FIELD; }

static const char *take_qual(P *p) {
  Token *c = next(p);
  return intern(xsprintf("%s.%s", c->s, next(p)->s));
}

/* ------------------------------------------------------------ operators */

typedef struct { const char *op; int prec; char assoc; } Fixity;
static const Fixity fixities[] = {
    {".", 9, 'r'},   {"!!", 9, 'l'},   {"^", 8, 'r'},    {"**", 8, 'r'},  {"*", 7, 'l'},
    {"/", 7, 'l'},   {"div", 7, 'l'},  {"mod", 7, 'l'},  {"quot", 7, 'l'}, {"rem", 7, 'l'},
    {"+", 6, 'l'},   {"-", 6, 'l'},    {":", 5, 'r'},    {"++", 5, 'r'},  {"==", 4, 'n'},
    {"/=", 4, 'n'},  {"<", 4, 'n'},    {"<=", 4, 'n'},   {">", 4, 'n'},   {">=", 4, 'n'},
    {"elem", 4, 'n'}, {"&&", 3, 'r'},  {"||", 2, 'r'},   {">>", 1, 'l'},  {"|>", 1, 'l'},
    {">>>", 1, 'r'}, {"$", 0, 'r'},   {"shiftL", 8, 'l'}, {"shiftR", 8, 'l'}, {".&.", 7, 'l'},
    {"xor", 6, 'l'}, {".|.", 5, 'l'}}; /* Data.Bits */

static Fixity fixity(const char *op) {
  const char *dot = islower((unsigned char)op[0]) ? strchr(op, '.') : NULL;
  if (dot && dot[1]) op = intern(dot + 1); /* q.op: an import's operator has its own fixity */
  int v = map_get(&ST->user_fixities, op);
  if (v >= 0) return (Fixity){op, v / 3, "lrn"[v % 3]};
  for (size_t i = 0; i < sizeof fixities / sizeof fixities[0]; i++)
    if (strcmp(fixities[i].op, op) == 0) return fixities[i];
  return (Fixity){op, 9, 'l'};
}

/* Length in tokens of the operator at the cursor: 1 for a symbol, 3 for
   `name`, 0 if none. */
static int op_len(P *p, int off) {
  Token *t = ahead(p, off);
  if (t->k == T_VARSYM || t->k == T_CONSYM) return 1;
  if (t->k == T_BACKTICK && (ahead(p, off + 1)->k == T_VARID || ahead(p, off + 1)->k == T_CONID) &&
      ahead(p, off + 2)->k == T_BACKTICK)
    return 3;
  return 0;
}

static Token *take_op(P *p) {
  int n = op_len(p, 0);
  Token *t = n == 3 ? ahead(p, 1) : cur(p);
  p->pos += n;
  return t;
}

typedef VEC(const char *) Names;

/* infixl N op, `name`, ... with N defaulting to 9. Appends the operators
   to names, which spans the module so duplicates are caught; with reg,
   also installs their fixity. */
static void parse_fixity(P *p, bool reg, Names *names) {
  Token *t = next(p);
  int prec = 9, assoc = t->k == T_INFIXL ? 0 : t->k == T_INFIXR ? 1 : 2;
  if (is(p, T_INT)) {
    Token *n = next(p);
    if (n->ival > 9) cerror(n->line, n->col, "precedence must be between 0 and 9");
    prec = (int)n->ival;
  }
  do {
    int ol = op_len(p, 0);
    if (!(ol == 1 && is(p, T_VARSYM)) && !(ol == 3 && ahead(p, 1)->k == T_VARID)) unexpected(p, "an operator");
    Token *op = take_op(p);
    for (int i = 0; i < names->n; i++)
      if (names->a[i] == op->s) cerror(op->line, op->col, "duplicate fixity declaration for `%s`", op->s);
    VPUSH(*names, op->s);
    if (reg) map_put(&ST->user_fixities, op->s, prec * 3 + assoc);
  } while (accept(p, T_COMMA));
}

static bool is_fixity(P *p) { return is(p, T_INFIXL) || is(p, T_INFIXR) || is(p, T_INFIX); }

/* Operator expressions may use a fixity declared later in the module, so
   all declarations are installed before parsing. Local ones are rejected
   later by parse_decl, so scanning ignores scope. */
static void prescan_fixities(P p) {
  Names names = {0};
  for (p.pos = 0; p.pos < p.n - 1; p.pos++)
    if (is_fixity(&p)) parse_fixity(&p, true, &names);
  xfree(names.a);
}

/* ----------------------------------------------------------------- nodes */

static Expr *mk(int k, int line, int col) {
  Expr *e = NEW(Expr);
  e->k = k;
  e->line = line;
  e->col = col;
  return e;
}

static Expr *mkvar(const char *name, int line, int col) {
  Expr *e = mk(E_VAR, line, col);
  e->name = intern(name);
  return e;
}

static Expr *mkcon(const char *name, int line, int col) {
  Expr *e = mk(E_CON, line, col);
  e->name = intern(name);
  return e;
}

static Expr *mkapp(Expr *f, int n, Expr **args) {
  Expr *e = mk(E_APP, f->line, f->col);
  e->fn = f;
  e->n = n;
  e->args = xmalloc((size_t)n * sizeof *args);
  copy_n(e->args, args, (size_t)n * sizeof *args);
  return e;
}

static Expr *app2(Expr *f, Expr *a, Expr *b) { return mkapp(f, 2, (Expr *[]){a, b}); }

static Expr *mklam(int n, const char **params, Expr *body, int line, int col) {
  Expr *e = mk(E_LAM, line, col);
  Lambda *L = NEW(Lambda);
  L->n = n;
  L->params = params;
  L->body = body;
  L->line = line;
  L->col = col;
  e->lam = L;
  return e;
}

static Expr *mkif(Expr *c, Expr *t, Expr *f) {
  Expr *e = mk(E_IF, c->line, c->col);
  e->cond = c;
  e->then_ = t;
  e->else_ = f;
  return e;
}

static Expr *op_expr(Token *t) {
  bool con = t->k == T_CONSYM || t->k == T_CONID;
  return con ? mkcon(t->s, t->line, t->col) : mkvar(t->s, t->line, t->col);
}

/* `l op r` is located at l, where the expression starts. */
static Expr *binop(Token *op, Expr *l, Expr *r) {
  Expr *e;
  if (op->s == intern("&&")) e = mkif(l, r, mkcon("False", op->line, op->col));
  else if (op->s == intern("||")) e = mkif(l, mkcon("True", op->line, op->col), r);
  else e = app2(op_expr(op), l, r);
  e->line = l->line;
  e->col = l->col;
  return e;
}

static Pat *mkpat(int k, int line, int col) {
  Pat *pt = NEW(Pat);
  pt->k = k;
  pt->line = line;
  pt->col = col;
  return pt;
}

static Pat *pcon(const char *name, int n, Pat **args, int line, int col) {
  Pat *pt = mkpat(P_CON, line, col);
  pt->name = intern(name);
  pt->n = n;
  pt->args = n ? xmalloc((size_t)n * sizeof *args) : NULL;
  if (n) memcpy(pt->args, args, (size_t)n * sizeof *args);
  return pt;
}

static Pat *pat_clone(Pat *p) {
  Pat *c = NEW(Pat);
  *c = *p;
  if (p->n) {
    c->args = xmalloc((size_t)p->n * sizeof *c->args);
    for (int i = 0; i < p->n; i++) c->args[i] = pat_clone(p->args[i]);
  }
  return c;
}

static const char *tuple_name(int n) {
  char buf[16] = "(";
  for (int i = 1; i < n; i++) strcat(buf, ",");
  strcat(buf, ")");
  return intern(buf);
}

static Rhs simple_rhs(Expr *body) {
  Rhs r = {0};
  r.ng = 1;
  r.guards = xcalloc(1, sizeof(Expr *));
  r.bodies = xmalloc(sizeof(Expr *));
  r.bodies[0] = body;
  return r;
}

static Expr *mkcase(int ns, Expr **scruts, int na, Alt **alts, int line, int col) {
  Expr *e = mk(E_CASE, line, col);
  e->name = CS->cur_file; /* for match-failure messages */
  e->ns = ns;
  e->scruts = scruts;
  e->na = na;
  e->alts = alts;
  return e;
}

static Alt *mkalt(int np, Pat **pats, Rhs rhs) {
  Alt *a = NEW(Alt);
  a->np = np;
  a->pats = pats;
  a->rhs = rhs;
  return a;
}

/* A guarded or where-carrying rhs becomes a zero-scrutinee case. */
static Expr *rhs_expr(Rhs *r, int line, int col) {
  if (r->ng == 1 && !r->guards[0] && r->nwg == 0) return r->bodies[0];
  Alt **alts = xmalloc(sizeof(Alt *));
  alts[0] = mkalt(0, NULL, *r);
  return mkcase(0, NULL, 1, alts, line, col);
}

/* \p1..pn -> body over one or more clauses. */
static Expr *match_lambda(int n, int k, Pat ***pats, Rhs *rhss, int line, int col) {
  bool simple = k == 1;
  for (int i = 0; simple && i < n; i++) {
    if (pats[0][i]->k != P_VAR) simple = false;
    for (int j = 0; simple && j < i; j++)
      if (pats[0][j]->name == pats[0][i]->name) simple = false;
  }
  const char **params = xmalloc((size_t)n * sizeof *params);
  if (simple) {
    for (int i = 0; i < n; i++) params[i] = pats[0][i]->name;
    return mklam(n, params, rhs_expr(&rhss[0], line, col), line, col);
  }
  Expr **scruts = xmalloc((size_t)n * sizeof *scruts);
  for (int i = 0; i < n; i++) {
    params[i] = fresh("a");
    scruts[i] = mkvar(params[i], line, col);
  }
  Alt **alts = xmalloc((size_t)k * sizeof *alts);
  for (int c = 0; c < k; c++) alts[c] = mkalt(n, pats[c], rhss[c]);
  return mklam(n, params, mkcase(n, scruts, k, alts, line, col), line, col);
}

/* ----------------------------------------------------------------- types */

static TypeExpr *mkte(int k, Token *at) {
  TypeExpr *t = NEW(TypeExpr);
  t->k = k;
  t->line = at->line;
  t->col = at->col;
  return t;
}

static bool atype_start(P *p) {
  TokKind k = cur(p)->k;
  return k == T_VARID || k == T_CONID || k == T_LPAREN || k == T_LBRACKET;
}

static TypeExpr *parse_atype(P *p) {
  Token *t = cur(p);
  if (accept(p, T_VARID)) {
    TypeExpr *te = mkte(TE_VAR, t);
    te->name = t->s;
    return te;
  }
  if (accept(p, T_CONID)) {
    TypeExpr *te = mkte(TE_CON, t);
    te->name = t->s;
    return te;
  }
  if (accept(p, T_LBRACKET)) {
    TypeExpr *te = mkte(TE_LIST, t);
    te->n = 1;
    te->args = xmalloc(sizeof *te->args);
    te->args[0] = parse_type(p);
    expect(p, T_RBRACKET, NULL);
    return te;
  }
  expect(p, T_LPAREN, "a type");
  VEC(TypeExpr *) el = {0};
  if (!is(p, T_RPAREN)) {
    do VPUSH(el, parse_type(p));
    while (accept(p, T_COMMA));
  }
  expect(p, T_RPAREN, NULL);
  if (el.n == 1) return el.a[0];
  TypeExpr *te = mkte(TE_TUPLE, t);
  te->n = el.n;
  te->args = el.a;
  return te;
}

static TypeExpr *parse_btype(P *p) {
  Token *t = cur(p);
  TypeExpr *head = parse_atype(p);
  if (!atype_start(p)) return head;
  if (head->k != TE_CON || head->n) cerror(t->line, t->col, "only type constructors can be applied");
  VEC(TypeExpr *) args = {0};
  while (atype_start(p)) VPUSH(args, parse_atype(p));
  head->n = args.n;
  head->args = args.a;
  return head;
}

static TypeExpr *parse_type(P *p) {
  Token *t = cur(p);
  TypeExpr *a = parse_btype(p);
  bool eff = cur(p)->k == T_VARSYM && strcmp(cur(p)->s, "->>") == 0; /* an effectful arrow (typed-effects.md, E) */
  if (eff) next(p);
  else if (!accept(p, T_ARROW)) return a;
  TypeExpr *f = mkte(TE_FUN, t);
  if (eff) f->name = intern("->>");
  f->n = 2;
  f->args = xmalloc(2 * sizeof *f->args);
  f->args[0] = a;
  f->args[1] = parse_type(p);
  return f;
}

/* [Num a =>] type. The context parses as a type first, then is checked. */
static Sig *parse_sig(P *p) {
  Token *t = cur(p);
  Sig *s = NEW(Sig);
  s->line = t->line;
  s->col = t->col;
  TypeExpr *te = parse_type(p);
  if (accept(p, T_DARROW)) {
    int n = te->k == TE_TUPLE ? te->n : 1;
    TypeExpr **cs = te->k == TE_TUPLE ? te->args : &te;
    s->ctx = xmalloc((size_t)n * sizeof *s->ctx);
    s->ctx_cls = xmalloc((size_t)n * sizeof *s->ctx_cls);
    for (int i = 0; i < n; i++) {
      TypeExpr *c = cs[i];
      if (c->k != TE_CON || c->n != 1 || c->args[0]->k != TE_VAR)
        cerror(c->line, c->col, "constraint must have the form `Class a`");
      s->ctx_cls[i] = c->name;
      s->ctx[i] = c->args[0]->name;
    }
    s->nctx = n;
    te = parse_type(p);
  }
  s->t = te;
  return s;
}

/* -------------------------------------------------------------- patterns */

static bool apat_start(P *p) {
  switch (cur(p)->k) {
  case T_VARID: case T_CONID: case T_UNDERSCORE: case T_INT: case T_CHAR:
  case T_STRING: case T_LPAREN: case T_LBRACKET: return true;
  default: return false;
  }
}

/* A string literal pattern matches a String equal to it. */
static Pat *string_pat(Token *t) {
  Pat *pt = mkpat(P_STR, t->line, t->col);
  pt->name = t->s;
  pt->ival = t->len;
  return pt;
}

static Pat *parse_apat(P *p) {
  Token *t = cur(p);
  switch (t->k) {
  case T_VARID: {
    next(p);
    Pat *pt = mkpat(P_VAR, t->line, t->col);
    pt->name = t->s;
    return pt;
  }
  case T_UNDERSCORE: next(p); return mkpat(P_WILD, t->line, t->col);
  case T_CONID: {
    next(p);
    if (!accept(p, T_LBRACE)) return pcon(t->s, 0, NULL, t->line, t->col);
    VEC(Pat *) ps = {0};
    VEC(const char *) fs = {0};
    if (!is(p, T_RBRACE)) {
      do {
        VPUSH(fs, expect(p, T_VARID, "a field name")->s);
        expect(p, T_EQ, NULL);
        VPUSH(ps, parse_pat(p));
      } while (accept(p, T_COMMA));
    }
    expect(p, T_RBRACE, "',' or '}'");
    Pat *pt = pcon(t->s, ps.n, ps.a, t->line, t->col);
    pt->rec = true;
    pt->fields = fs.a;
    return pt;
  }
  case T_INT: {
    next(p);
    Pat *pt = mkpat(P_INT, t->line, t->col);
    pt->ival = t->ival;
    return pt;
  }
  case T_CHAR: {
    next(p);
    Pat *pt = mkpat(P_CHAR, t->line, t->col);
    pt->ival = t->ival;
    return pt;
  }
  case T_STRING: next(p); return string_pat(t);
  case T_LBRACKET: {
    next(p);
    VEC(Pat *) el = {0};
    if (!is(p, T_RBRACKET)) {
      do VPUSH(el, parse_pat(p));
      while (accept(p, T_COMMA));
    }
    expect(p, T_RBRACKET, NULL);
    Pat *pt = pcon("[]", 0, NULL, t->line, t->col);
    for (int i = el.n; i-- > 0;) pt = pcon(":", 2, (Pat *[]){el.a[i], pt}, t->line, t->col);
    return pt;
  }
  case T_LPAREN: {
    next(p);
    if (accept(p, T_RPAREN)) return pcon("()", 0, NULL, t->line, t->col);
    VEC(Pat *) el = {0};
    do VPUSH(el, parse_pat(p));
    while (accept(p, T_COMMA));
    expect(p, T_RPAREN, NULL);
    if (el.n == 1) return el.a[0];
    if (el.n > FK_MAXTUPLE) cerror(t->line, t->col, "tuples have at most %d elements", FK_MAXTUPLE);
    return pcon(tuple_name(el.n), el.n, el.a, t->line, t->col);
  }
  case T_FLOAT: cerror(t->line, t->col, "float literals are not allowed in patterns");
  default: unexpected(p, "a pattern");
  }
}

static Pat *parse_lpat(P *p) {
  Token *t = cur(p);
  if (t->k == T_VARSYM && t->s == intern("-") && ahead(p, 1)->k == T_INT) {
    next(p);
    Pat *pt = mkpat(P_INT, t->line, t->col);
    pt->ival = -next(p)->ival;
    return pt;
  }
  if (t->k == T_CONID && ahead(p, 1)->k != T_LBRACE) {
    next(p);
    VEC(Pat *) args = {0};
    while (apat_start(p)) VPUSH(args, parse_apat(p));
    return pcon(t->s, args.n, args.a, t->line, t->col);
  }
  return parse_apat(p);
}

static Pat *parse_pat(P *p) {
  Pat *l = parse_lpat(p);
  Token *t = cur(p);
  if (t->k == T_CONSYM && t->s == intern(":")) {
    next(p);
    return pcon(":", 2, (Pat *[]){l, parse_pat(p)}, t->line, t->col);
  }
  return l;
}

/* ----------------------------------------------------------- expressions */

static bool aexp_start(P *p) {
  switch (cur(p)->k) {
  case T_VARID: case T_CONID: case T_INT: case T_FLOAT: case T_CHAR:
  case T_STRING: case T_LPAREN: case T_LBRACKET: return true;
  default: return false;
  }
}

static Expr *parse_op(P *p, int minprec);

static Expr *parse_paren(P *p) {
  Token *lp = expect(p, T_LPAREN, NULL);
  if (accept(p, T_RPAREN)) return mkcon("()", lp->line, lp->col);
  if (is(p, T_FIELD)) { /* (.x.y) is \r -> r.x.y */
    const char **param = xmalloc(sizeof *param);
    param[0] = fresh("r");
    Expr *e = mkvar(param[0], lp->line, lp->col);
    while (is(p, T_FIELD)) {
      Token *f = next(p);
      Expr *fe = mk(E_FIELD, f->line, f->col);
      fe->body = e;
      fe->name = f->s;
      e = fe;
    }
    expect(p, T_RPAREN, NULL);
    return mklam(1, param, e, lp->line, lp->col);
  }
  int ol = op_len(p, 0);
  bool neg = cur(p)->k == T_VARSYM && cur(p)->s == intern("-");
  if (ol && !(neg && ahead(p, 1)->k != T_RPAREN)) {
    Token *op = take_op(p);
    if (accept(p, T_RPAREN)) return op_expr(op);
    Expr *e = parse_expr(p);
    expect(p, T_RPAREN, NULL);
    return app2(mkvar("flip", op->line, op->col), op_expr(op), e); /* (op e) */
  }
  if (is(p, T_COMMA)) {
    int n = 1;
    while (accept(p, T_COMMA)) n++;
    expect(p, T_RPAREN, NULL);
    return mkcon(tuple_name(n), lp->line, lp->col);
  }
  Expr *e = parse_op(p, 0);
  if (op_len(p, 0)) { /* (e op) */
    Token *op = take_op(p);
    expect(p, T_RPAREN, NULL);
    return mkapp(op_expr(op), 1, &e);
  }
  if (accept(p, T_DCOLON)) {
    Expr *a = mk(E_ANN, e->line, e->col);
    a->body = e;
    a->sig = parse_sig(p);
    e = a;
  }
  if (!is(p, T_COMMA)) {
    expect(p, T_RPAREN, NULL);
    return e;
  }
  VEC(Expr *) el = {0};
  VPUSH(el, e);
  while (accept(p, T_COMMA)) VPUSH(el, parse_expr(p));
  expect(p, T_RPAREN, NULL);
  if (el.n > FK_MAXTUPLE) cerror(lp->line, lp->col, "tuples have at most %d elements", FK_MAXTUPLE);
  return mkapp(mkcon(tuple_name(el.n), lp->line, lp->col), el.n, el.a);
}

static Expr *parse_list(P *p) {
  Token *lb = expect(p, T_LBRACKET, NULL);
  int line = lb->line, col = lb->col;
  if (accept(p, T_RBRACKET)) return mkcon("[]", line, col);
  Expr *first = parse_expr(p);
  if (accept(p, T_DOTDOT)) {
    Expr *last = parse_expr(p);
    expect(p, T_RBRACKET, NULL);
    return app2(mkvar("enumFromTo", line, col), first, last);
  }
  VEC(Expr *) el = {0};
  VPUSH(el, first);
  while (accept(p, T_COMMA)) {
    VPUSH(el, parse_expr(p));
    if (el.n == 2 && accept(p, T_DOTDOT)) {
      Expr *last = parse_expr(p);
      expect(p, T_RBRACKET, NULL);
      return mkapp(mkvar("enumFromThenTo", line, col), 3, (Expr *[]){el.a[0], el.a[1], last});
    }
  }
  expect(p, T_RBRACKET, "',' or ']'");
  Expr *e = mkcon("[]", line, col);
  for (int i = el.n; i-- > 0;) e = app2(mkcon(":", line, col), el.a[i], e);
  return e;
}

static Expr *parse_aexp0(P *p) {
  Token *t = cur(p);
  Expr *e;
  switch (t->k) {
  case T_VARID: next(p); return mkvar(t->s, t->line, t->col);
  case T_CONID:
    if (qual_start(p, 0)) return mkvar(take_qual(p), t->line, t->col);
    next(p);
    return mkcon(t->s, t->line, t->col);
  case T_INT:
    next(p);
    e = mk(E_INT, t->line, t->col);
    e->ival = t->ival;
    return e;
  case T_FLOAT:
    next(p);
    e = mk(E_FLOAT, t->line, t->col);
    e->fval = t->fval;
    return e;
  case T_CHAR:
    next(p);
    e = mk(E_CHAR, t->line, t->col);
    e->ival = t->ival;
    return e;
  case T_STRING:
    next(p);
    e = mk(E_STR, t->line, t->col);
    e->name = t->s;
    e->slen = t->len;
    return e;
  case T_LPAREN: return parse_paren(p);
  case T_LBRACKET: return parse_list(p);
  default: unexpected(p, "an expression");
  }
}

/* `{ f = e, ... }` after base: construction if base is a constructor. */
static Expr *parse_record(P *p, Expr *base) {
  Token *t = expect(p, T_LBRACE, NULL);
  VEC(Expr *) es = {0};
  VEC(const char *) fs = {0};
  if (!is(p, T_RBRACE)) {
    do {
      VPUSH(fs, expect(p, T_VARID, "a field name")->s);
      expect(p, T_EQ, NULL);
      VPUSH(es, parse_expr(p));
    } while (accept(p, T_COMMA));
  }
  expect(p, T_RBRACE, "',' or '}'");
  Expr *e = mk(E_RECORD, t->line, t->col);
  e->fn = base;
  e->n = es.n;
  e->args = es.a;
  e->fields = fs.a;
  return e;
}

/* Field access and record braces bind tighter than application. */
static Expr *parse_aexp(P *p) {
  Expr *e = parse_aexp0(p);
  for (;;) {
    Token *t = cur(p);
    if (accept(p, T_FIELD)) {
      Expr *f = mk(E_FIELD, t->line, t->col);
      f->body = e;
      f->name = t->s;
      e = f;
    } else if (is(p, T_LBRACE)) {
      e = parse_record(p, e);
    } else {
      return e;
    }
  }
}

static Alt *parse_alt(P *p);

static Rhs parse_rhs(P *p, TokKind sep, bool allow_where) {
  Rhs r = {0};
  VEC(Expr *) gs = {0};
  VEC(Expr *) bs = {0};
  if (accept(p, sep)) {
    VPUSH(gs, NULL);
    VPUSH(bs, parse_expr(p));
  } else if (is(p, T_BAR)) {
    while (accept(p, T_BAR)) {
      VPUSH(gs, parse_expr(p));
      expect(p, sep, NULL);
      VPUSH(bs, parse_expr(p));
    }
  } else {
    unexpected(p, sep == T_EQ ? "'=' or '|'" : "'->' or '|'");
  }
  r.ng = gs.n;
  r.guards = gs.a;
  r.bodies = bs.a;
  if (allow_where && accept(p, T_WHERE)) {
    Binds wb = {0};
    parse_decl_block(p, &wb);
    if (wb.n) {
      r.nwg = 1;
      r.wg = NEW(Group);
      r.wg->b = wb.a;
      r.wg->n = wb.n;
    }
  }
  return r;
}

/* ListItems of an explicit or layout block, separated by ';'. */
#define BLOCK(p, item)                                                         \
  do {                                                                         \
    Token *open_ = cur(p);                                                     \
    bool virt_ = open_->k == T_VLBRACE;                                        \
    if (!virt_) expect(p, T_LBRACE, "a block");                                \
    else next(p);                                                              \
    TokKind close_ = virt_ ? T_VRBRACE : T_RBRACE;                             \
    for (;;) {                                                                 \
      while (accept(p, T_SEMI)) {}                                             \
      if (accept(p, close_)) break;                                            \
      item;                                                                    \
      if (accept(p, close_)) break;                                            \
      expect(p, T_SEMI, virt_ ? "a new line at the block's indentation" : "';' or '}'"); \
    }                                                                          \
  } while (0)

/* A statement: `p <- e`, an expression (pat NULL), or `let decls`
   (e NULL). */
typedef struct { Expr *e; Pat *pat; Binds let; int line, col; } Stmt;

/* Does the statement at the cursor contain `<-` outside brackets? */
static bool bind_stmt(P *p) {
  for (int i = 0, depth = 0;; i++) {
    switch (ahead(p, i)->k) {
    case T_LPAREN: case T_LBRACKET: case T_LBRACE: case T_VLBRACE: depth++; break;
    case T_RPAREN: case T_RBRACKET: case T_RBRACE: case T_VRBRACE:
      if (--depth < 0) return false;
      break;
    case T_SEMI: if (!depth) return false; break;
    case T_LARROW: if (!depth) return true; break;
    case T_EOF: return false;
    default: break;
    }
  }
}

static Stmt parse_stmt(P *p) {
  Token *t = cur(p);
  Stmt s = {NULL, NULL, {0}, t->line, t->col};
  if (bind_stmt(p)) {
    s.pat = parse_pat(p);
    expect(p, T_LARROW, NULL);
    s.e = parse_expr(p);
    return s;
  }
  if (!is(p, T_LET)) {
    s.e = parse_expr(p);
    s.pat = mkpat(P_WILD, t->line, t->col);
    return s;
  }
  next(p);
  parse_decl_block(p, &s.let);
  if (!accept(p, T_IN)) return s;
  Expr *e = mk(E_LET, t->line, t->col); /* let ... in e as an expression statement */
  e->g.b = s.let.a;
  e->g.n = s.let.n;
  e->body = parse_expr(p);
  s.e = s.let.n ? e : e->body;
  s.pat = mkpat(P_WILD, t->line, t->col);
  s.let = (Binds){0};
  return s;
}

/* Each statement is a `case` on its expression, so the expression runs
   before the rest of the block, which stays in tail position. */
static Expr *parse_do(P *p, Token *t) {
  VEC(Stmt) ss = {0};
  BLOCK(p, VPUSH(ss, parse_stmt(p)));
  if (!ss.n) cerror(t->line, t->col, "empty `do` block");
  Stmt *last = &ss.a[ss.n - 1];
  if (!last->e || last->pat->k != P_WILD)
    cerror(last->line, last->col, "the last statement of a `do` block must be an expression");
  Expr *e = last->e;
  for (int i = ss.n - 1; i-- > 0;) {
    Stmt *s = &ss.a[i];
    if (s->e) {
      Expr **sc = xmalloc(sizeof *sc);
      sc[0] = s->e;
      Pat **ps = xmalloc(sizeof *ps);
      ps[0] = s->pat;
      Alt **alts = xmalloc(sizeof *alts);
      alts[0] = mkalt(1, ps, simple_rhs(e));
      e = mkcase(1, sc, 1, alts, s->line, s->col);
    } else if (s->let.n) {
      for (int k = 0; k < s->let.n; k++) s->let.a[k]->stmt = true;
      Expr *l = mk(E_LET, s->line, s->col);
      l->g.b = s->let.a;
      l->g.n = s->let.n;
      l->body = e;
      e = l;
    }
  }
  return e;
}

static Expr *parse_exp10(P *p) {
  Token *t = cur(p);
  if (accept(p, T_LAMBDA)) {
    VEC(Pat *) ps = {0};
    while (apat_start(p)) VPUSH(ps, parse_apat(p));
    if (!ps.n) unexpected(p, "a lambda parameter");
    expect(p, T_ARROW, NULL);
    Rhs r = simple_rhs(parse_expr(p));
    return match_lambda(ps.n, 1, &ps.a, &r, t->line, t->col);
  }
  if (accept(p, T_LET)) {
    Binds bs = {0};
    parse_decl_block(p, &bs);
    expect(p, T_IN, "'in'");
    Expr *body = parse_expr(p);
    if (!bs.n) return body;
    Expr *e = mk(E_LET, t->line, t->col);
    e->g.b = bs.a;
    e->g.n = bs.n;
    e->body = body;
    return e;
  }
  if (accept(p, T_DO)) return parse_do(p, t);
  if (accept(p, T_IF)) {
    Expr *c = parse_expr(p);
    accept(p, T_SEMI);
    expect(p, T_THEN, NULL);
    Expr *a = parse_expr(p);
    accept(p, T_SEMI);
    expect(p, T_ELSE, NULL);
    Expr *e = mkif(c, a, parse_expr(p));
    e->line = t->line;
    e->col = t->col;
    return e;
  }
  if (accept(p, T_CASE)) {
    Expr *s = parse_expr(p);
    expect(p, T_OF, NULL);
    VEC(Alt *) alts = {0};
    BLOCK(p, VPUSH(alts, parse_alt(p)));
    if (!alts.n) cerror(t->line, t->col, "case with no alternatives");
    Expr **sc = xmalloc(sizeof *sc);
    sc[0] = s;
    return mkcase(1, sc, alts.n, alts.a, t->line, t->col);
  }
  Expr *f = parse_aexp(p);
  VEC(Expr *) args = {0};
  while (aexp_start(p)) VPUSH(args, parse_aexp(p));
  return args.n ? mkapp(f, args.n, args.a) : f;
}

static Alt *parse_alt(P *p) {
  Pat **pats = xmalloc(sizeof *pats);
  pats[0] = parse_pat(p);
  return mkalt(1, pats, parse_rhs(p, T_ARROW, false));
}

/* Precedence climbing. Stops before an operator followed by ')' so the
   caller can build a left section. */
static Expr *parse_op(P *p, int minprec) {
  Expr *lhs;
  Token *t = cur(p);
  if (t->k == T_VARSYM && t->s == intern("-")) {
    next(p);
    Expr *x = parse_op(p, 7);
    if (x->k == E_INT || x->k == E_FLOAT) {
      x->ival = -x->ival;
      x->fval = -x->fval;
      lhs = x;
    } else {
      lhs = mkapp(mkvar("negate", t->line, t->col), 1, &x);
    }
  } else {
    lhs = parse_exp10(p);
  }
  for (;;) {
    int ol = op_len(p, 0);
    if (!ol || ahead(p, ol)->k == T_RPAREN) break;
    Fixity f = fixity((ol == 3 ? ahead(p, 1) : cur(p))->s);
    if (f.prec < minprec) break;
    Token *op = take_op(p);
    Expr *rhs = parse_op(p, f.assoc == 'r' ? f.prec : f.prec + 1);
    lhs = binop(op, lhs, rhs);
    if (f.assoc == 'n') {
      int ol2 = op_len(p, 0);
      if (ol2 && fixity((ol2 == 3 ? ahead(p, 1) : cur(p))->s).prec == f.prec && ahead(p, ol2)->k != T_RPAREN)
        cerror(cur(p)->line, cur(p)->col, "cannot chain non-associative operators of precedence %d; add parentheses", f.prec);
    }
  }
  return lhs;
}

static Expr *parse_expr(P *p) {
  Expr *e = parse_op(p, 0);
  if (accept(p, T_DCOLON)) {
    Expr *a = mk(E_ANN, e->line, e->col);
    a->body = e;
    a->sig = parse_sig(p);
    return a;
  }
  return e;
}

/* ---------------------------------------------------------- declarations */

static DataDecl *parse_data(P *p) {
  Token *t = expect(p, T_DATA, NULL);
  DataDecl *d = NEW(DataDecl);
  d->line = t->line;
  d->col = t->col;
  d->name = expect(p, T_CONID, "a type name")->s;
  VEC(const char *) ps = {0};
  while (is(p, T_VARID)) VPUSH(ps, next(p)->s);
  d->np = ps.n;
  d->params = ps.a;
  if (!is(p, T_EQ)) return d; /* opaque: no values, for `Ptr Db` in foreign imports */
  expect(p, T_EQ, NULL);
  VEC(ConDecl) cs = {0};
  do {
    Token *c = expect(p, T_CONID, "a constructor name");
    ConDecl cd = {c->s, 0, NULL, NULL, c->line, c->col};
    VEC(TypeExpr *) fs = {0};
    if (accept(p, T_LBRACE)) { /* C { a, b :: T, c :: U } */
      VEC(const char *) ls = {0};
      if (!is(p, T_RBRACE)) {
        do {
          do VPUSH(ls, expect(p, T_VARID, "a field name")->s);
          while (accept(p, T_COMMA));
          expect(p, T_DCOLON, NULL);
          TypeExpr *ty = parse_type(p);
          while (fs.n < ls.n) VPUSH(fs, ty);
        } while (accept(p, T_COMMA));
      }
      expect(p, T_RBRACE, "',' or '}'");
      cd.labels = ls.n ? ls.a : NULL;
    }
    while (!cd.labels && atype_start(p)) VPUSH(fs, parse_atype(p));
    cd.nfields = fs.n;
    cd.fields = fs.a;
    VPUSH(cs, cd);
  } while (accept(p, T_BAR));
  d->nc = cs.n;
  d->cons = cs.a;
  d->derive_line = cur(p)->line;
  d->derive_col = cur(p)->col;
  if (accept(p, T_DERIVING)) {
    VEC(const char *) ds = {0};
    if (accept(p, T_LPAREN)) {
      if (!is(p, T_RPAREN)) {
        do VPUSH(ds, expect(p, T_CONID, "a class name")->s);
        while (accept(p, T_COMMA));
      }
      expect(p, T_RPAREN, NULL);
    } else {
      VPUSH(ds, expect(p, T_CONID, "a class name")->s);
    }
    d->nderiv = ds.n;
    d->derivs = ds.a;
  }
  return d;
}

/* ------------------------------------------------------------- deriving */

/* Derived Eq, Ord and Show instances are generated as source text, one
   line with explicit braces, then parsed. Their tokens take the location
   of the `deriving` clause, so errors in them point there. */

static bool tuple_con(const char *s) { return s[0] == '(' && s[1] == ','; }

static void con_pat_text(SB *b, ConDecl *c, char var) {
  if (tuple_con(c->name)) {
    for (int i = 0; i < c->nfields; i++) sb_printf(b, "%s%c%d", i ? ", " : "(", var, i);
    sb_puts(b, ")");
    return;
  }
  if (!c->nfields) { sb_puts(b, c->name); return; }
  sb_printf(b, "(%s", c->name);
  for (int i = 0; i < c->nfields; i++) sb_printf(b, " %c%d", var, i);
  sb_puts(b, ")");
}

static char *derive_text(DataDecl *d, const char *cls) {
  SB b = {0};
  sb_printf(&b, "instance ");
  for (int i = 0; i < d->np; i++) sb_printf(&b, "%s%s %s", i ? ", " : "(", cls, d->params[i]);
  if (d->np) sb_puts(&b, ") => ");
  sb_printf(&b, "%s ", cls);
  if (tuple_con(d->name)) {
    for (int i = 0; i < d->np; i++) sb_printf(&b, "%s%s", i ? ", " : "(", d->params[i]);
    sb_printf(&b, ")");
  } else if (d->np) {
    sb_printf(&b, "(%s", d->name);
    for (int i = 0; i < d->np; i++) sb_printf(&b, " %s", d->params[i]);
    sb_puts(&b, ")");
  } else {
    sb_puts(&b, d->name);
  }
  sb_puts(&b, " where { ");
  for (int c = 0; c < d->nc; c++) {
    ConDecl *cd = &d->cons[c];
    int n = cd->nfields;
    if (strcmp(cls, "Show") == 0) {
      sb_puts(&b, "showPrec d ");
      con_pat_text(&b, cd, 'a');
      if (tuple_con(cd->name)) {
        sb_puts(&b, " = \"(\"");
        for (int i = 0; i < n; i++) sb_printf(&b, " ++ %sshowPrec 0 a%d", i ? "\",\" ++ " : "", i);
        sb_puts(&b, " ++ \")\"");
      } else if (!n) {
        sb_printf(&b, " = \"%s\"", cd->name);
      } else if (cd->labels) {
        sb_printf(&b, " = showParen (d > 10) (\"%s {", cd->name);
        for (int i = 0; i < n; i++) sb_printf(&b, "%s%s = \" ++ showPrec 0 a%d ++ \"", i ? ", " : "", cd->labels[i], i);
        sb_puts(&b, "}\")");
      } else {
        sb_printf(&b, " = showParen (d > 10) (\"%s\"", cd->name);
        for (int i = 0; i < n; i++) sb_printf(&b, " ++ \" \" ++ showPrec 11 a%d", i);
        sb_puts(&b, ")");
      }
    } else {
      sb_puts(&b, strcmp(cls, "Eq") == 0 ? "(==) " : "compare ");
      con_pat_text(&b, cd, 'a');
      sb_puts(&b, " ");
      con_pat_text(&b, cd, 'b');
      sb_puts(&b, " = ");
      if (strcmp(cls, "Eq") == 0) {
        for (int i = 0; i < n; i++) sb_printf(&b, "(a%d == b%d) && ", i, i);
        sb_puts(&b, "True");
      } else {
        for (int i = 0; i < n; i++) sb_printf(&b, "case compare a%d b%d of { EQ -> ", i, i);
        sb_puts(&b, "EQ");
        for (int i = 0; i < n; i++) sb_puts(&b, " ; o -> o }");
      }
    }
    sb_puts(&b, " ; ");
  }
  if (d->nc > 1 && strcmp(cls, "Eq") == 0) sb_puts(&b, "(==) _ _ = False ; ");
  if (d->nc > 1 && strcmp(cls, "Ord") == 0) sb_puts(&b, "compare x y = compare (primConTag x) (primConTag y) ; ");
  sb_puts(&b, "}");
  return b.s;
}

/* Parse generated top-level declarations, located at line:col. */
static void parse_generated(const char *src, Decls *out, int line, int col) {
  int n;
  Token *t = lex(src, strlen(src), &n);
  for (int i = 0; i < n; i++) {
    t[i].line = line;
    t[i].col = col;
  }
  P q = {t, n, 0};
  while (!is(&q, T_EOF)) {
    if (accept(&q, T_SEMI)) continue;
    parse_decl(&q, out, true);
  }
}

static void derive(DataDecl *d, Decls *out) {
  for (int i = 0; i < d->nderiv; i++) {
    const char *c = d->derivs[i];
    if (strcmp(c, "Eq") && strcmp(c, "Ord") && strcmp(c, "Show"))
      cerror(d->derive_line, d->derive_col, "cannot derive `%s`; only Eq, Ord and Show can be derived", c);
    parse_generated(derive_text(d, c), out, d->derive_line, d->derive_col);
  }
}

/* Instances for builtin types: Bool, (), tuples. Prepended to the prelude. */
char *builtin_instances_src(void) {
  static const char *pn[] = {"a", "b", "c", "d", "e", "f", "g"};
  SB b = {0};
  sb_puts(&b, "");
  DataDecl bl = {.name = "Bool", .nc = 2, .cons = (ConDecl[]){{.name = "False"}, {.name = "True"}}};
  DataDecl un = {.name = "()", .nc = 1, .cons = (ConDecl[]){{.name = "()"}}};
  DataDecl *ds[2] = {&bl, &un};
  for (int k = 0; k < 2 + FK_MAXTUPLE - 1; k++) {
    DataDecl tu = {0};
    ConDecl tc = {0};
    DataDecl *d = k < 2 ? ds[k] : &tu;
    if (k >= 2) {
      int n = k;
      SB nm = {0};
      sb_puts(&nm, "(");
      for (int i = 1; i < n; i++) sb_puts(&nm, ",");
      sb_puts(&nm, ")");
      tc.name = nm.s;
      tc.nfields = n;
      tu = (DataDecl){.name = nm.s, .np = n, .params = pn, .nc = 1, .cons = &tc};
    }
    for (int c = 0; c < 3; c++) {
      sb_puts(&b, derive_text(d, (const char *[]){"Eq", "Ord", "Show"}[c]));
      sb_puts(&b, "\n");
    }
  }
  /* The C types of foreign imports, held as Int or Float words that their
     arithmetic wraps to width. CULong and CSize compare and show unsigned. */
  for (int i = 0; i < ffi_ntypes; i++) {
    const char *t = ffi_types[i].name;
    if (t[0] != 'C' || !strcmp(t, "Char") || !strchr("if", ffi_types[i].kind)) continue;
    if (ffi_types[i].kind == 'f') {
      sb_printf(&b, "instance Eq %s where\n  x == y = fromCFloat x == fromCFloat y\n  x /= y = fromCFloat x /= fromCFloat y\n"
                    "instance Ord %s where\n  compare x y = compare (fromCFloat x) (fromCFloat y)\n"
                    "instance Show %s where\n  showPrec d x = if d > 6 && (fromCFloat x < 0.0 || (fromCFloat x == 0.0 && 1.0 / fromCFloat x < 0.0)) then \"(\" ++ primShowCFloat x ++ \")\" else primShowCFloat x\n"
                    "instance Num %s where\n  x + y = toCFloat (fromCFloat x + fromCFloat y)\n  x - y = toCFloat (fromCFloat x - fromCFloat y)\n"
                    "  x * y = toCFloat (fromCFloat x * fromCFloat y)\n  negate x = toCFloat (negate (fromCFloat x))\n"
                    "  abs x = toCFloat (abs (fromCFloat x))\n  fromInt n = toCFloat (toFloat n)\n"
                    "instance Storable %s where\n  sizeOf _ = 4\n  peek p = toCFloat (primPeekFloat 4 p)\n  poke p x = primPokeFloat 4 p (fromCFloat x)\n"
                    "instance Atomic %s where\n  atomicPeek p = toCFloat (primAtomicPeekFloat 4 p)\n  atomicPoke p x = primAtomicPokeFloat 4 p (fromCFloat x)\n"
                    "  atomicAdd p x = toCFloat (primAtomicAddFloat 4 p (fromCFloat x))\n"
                    "  atomicCompareSwap p old x = primAtomicCasFloat 4 p (fromCFloat old) (fromCFloat x)\n"
                    "instance FromFloat %s where\n  fromFloat x = toCFloat x\n",
                t, t, t, t, t, t, t);
      continue;
    }
    bool word = !strcmp(t, "CULong") || !strcmp(t, "CSize"), sign = t[1] != 'U' && !word;
    sb_printf(&b, "instance Eq %s where\n  x == y = primEqInt (primFrom%s x) (primFrom%s y)\n  x /= y = primNeInt (primFrom%s x) (primFrom%s y)\n", t, t, t, t, t);
    if (word)
      sb_printf(&b, "instance Ord %s where\n  compare x y = if primLtWord (primFrom%s x) (primFrom%s y) then LT else if x == y then EQ else GT\n"
                    "instance Show %s where\n  showPrec _ x = primShowWord (primFrom%s x)\n", t, t, t, t, t);
    else
      sb_printf(&b, "instance Ord %s where\n  compare x y = compare (primFrom%s x) (primFrom%s y)\n"
                    "instance Show %s where\n  showPrec d x = showPrec d (primFrom%s x)\n", t, t, t, t, t);
    sb_printf(&b, "instance Num %s where\n", t);
    for (int k = 0; k < 3; k++) {
      const char *op = (const char *[]){"+", "-", "*"}[k];
      sb_printf(&b, "  x %s y = primTo%s (primFrom%s x %s primFrom%s y)\n", op, t, t, op, t);
    }
    sb_printf(&b, "  negate x = primTo%s (negate (primFrom%s x))\n", t, t);
    if (sign) sb_printf(&b, "  abs x = primTo%s (abs (primFrom%s x))\n", t, t);
    else sb_puts(&b, "  abs x = x\n");
    sb_printf(&b, "  fromInt n = primTo%s n\ninstance Integral %s where\n  toInt n = primFrom%s n\n", t, t, t);
    sb_printf(&b, "instance Storable %s where\n  sizeOf _ = %d\n  peek p = primTo%s (primPeekInt (%d) p)\n  poke p x = primPokeInt (%d) p (primFrom%s x)\n", t,
              abs(ffi_types[i].width), t, ffi_types[i].width, ffi_types[i].width, t);
    int w = ffi_types[i].width;
    sb_printf(&b, "instance Atomic %s where\n  atomicPeek p = primTo%s (primAtomicPeekInt (%d) p)\n  atomicPoke p x = primAtomicPokeInt (%d) p (primFrom%s x)\n"
                  "  atomicAdd p x = primTo%s (primAtomicAddInt (%d) p (primFrom%s x))\n"
                  "  atomicCompareSwap p old x = primAtomicCasInt (%d) p (primFrom%s old) (primFrom%s x)\n",
              t, t, w, w, t, t, w, t, w, t, t);
  }
  return b.s;
}

/* class [ctx =>] C a where { sigs; default methods } */
static ClassDecl *parse_class(P *p) {
  Token *t = next(p);
  ClassDecl *c = NEW(ClassDecl);
  c->line = t->line;
  c->col = t->col;
  Sig *h = parse_sig(p);
  if (h->t->k != TE_CON || h->t->n != 1 || h->t->args[0]->k != TE_VAR)
    cerror(h->line, h->col, "a class declaration has the form `class C a`");
  c->name = h->t->name;
  c->var = h->t->args[0]->name;
  for (int i = 0; i < h->nctx; i++)
    if (h->ctx[i] != c->var) cerror(h->line, h->col, "a superclass must constrain `%s`", c->var);
  c->nsup = h->nctx;
  c->sups = h->ctx_cls;
  if (!accept(p, T_WHERE)) return c;
  Decls ds = {0}, defs = {0};
  BLOCK(p, parse_decl(p, &ds, false));
  VEC(const char *) names = {0};
  VEC(Sig *) sigs = {0};
  for (int i = 0; i < ds.n; i++) {
    Decl *d = &ds.a[i];
    if (d->k == D_SIG) {
      if (d->sig->nctx) cerror(d->line, d->col, "method signatures cannot have their own constraints");
      for (int k = 0; k < d->nn; k++) {
        VPUSH(names, d->names[k]);
        VPUSH(sigs, d->sig);
      }
    } else if (d->k == D_CLAUSE) {
      VPUSH(defs, *d);
    } else {
      cerror(d->line, d->col, "a class may contain only method signatures and default methods");
    }
  }
  Binds bs = {0};
  Datas none = {0};
  build_binds(&defs, &bs, &none);
  c->nsig = names.n;
  c->names = names.a;
  c->sigs = sigs.a;
  c->ndef = bs.n;
  c->defs = bs.a;
  return c;
}

/* instance [ctx =>] C T where { methods } */
static InstDecl *parse_instance(P *p) {
  Token *t = next(p);
  InstDecl *in = NEW(InstDecl);
  in->line = t->line;
  in->col = t->col;
  Sig *h = parse_sig(p);
  if (h->t->k != TE_CON || h->t->n != 1)
    cerror(h->line, h->col, "an instance declaration has the form `instance C T`");
  in->cls = h->t->name;
  in->head = h->t->args[0];
  in->ctx = h;
  if (!accept(p, T_WHERE)) return in;
  Binds bs = {0};
  parse_decl_block(p, &bs);
  for (int i = 0; i < bs.n; i++)
    if (bs.a[i]->sig) cerror(bs.a[i]->line, bs.a[i]->col, "signatures are not allowed in instances");
  in->nb = bs.n;
  in->binds = bs.a;
  return in;
}

static bool sig_start(P *p) {
  int i = 0;
  for (;;) {
    if (ahead(p, i)->k == T_VARID) i++;
    else if (qual_start(p, i)) i += 2;
    else if (ahead(p, i)->k == T_LPAREN && op_len(p, i + 1) == 1 && ahead(p, i + 2)->k == T_RPAREN) i += 3;
    else return false;
    if (ahead(p, i)->k == T_DCOLON) return true;
    if (ahead(p, i)->k != T_COMMA) return false;
    i++;
  }
}

/* `foreign import` or `foreign library`; `foreign` is a keyword only here. */
static bool foreign_start(Token *t) {
  return t[0].k == T_VARID && !strcmp(t[0].s, "foreign") && t[1].k == T_VARID &&
         (!strcmp(t[1].s, "import") || !strcmp(t[1].s, "export") || !strcmp(t[1].s, "library") || !strcmp(t[1].s, "struct"));
}

static void parse_foreign(P *p, Decl *d) {
  Token *t = next(p); /* foreign */
  const char *what = next(p)->s;
  if (!strcmp(what, "struct")) {
    StructDecl *s = NEW(StructDecl);
    s->line = t->line;
    s->col = t->col;
    if (is(p, T_STRING)) { /* "header.h ctype": C's struct, checked against the header */
      Token *ent = next(p);
      char *e = xsprintf("%.*s", ent->len, ent->s), *sp = strchr(e, ' ');
      if (!sp || !sp[1]) cerror(ent->line, ent->col, "a foreign struct names a header and a C type: \"time.h struct timespec\"");
      *sp = 0;
      s->header = intern(e);
      s->ctype = intern(sp + 1);
    }
    s->name = expect(p, T_CONID, "the struct's type name")->s;
    expect(p, T_LBRACE, "`{` and the fields");
    VEC(StructField) fs = {0};
    do {
      StructField f = {expect(p, T_VARID, "a field name")->s, NULL, NULL};
      if (is(p, T_STRING)) { /* the C member, if not the field's name: "on_message.callback" */
        Token *c = next(p);
        f.cpath = intern(xsprintf("%.*s", c->len, c->s));
      }
      expect(p, T_DCOLON, NULL);
      f.type = parse_type(p);
      VPUSH(fs, f);
    } while (accept(p, T_COMMA));
    expect(p, T_RBRACE, "',' or '}'");
    s->fields = fs.a;
    s->nf = fs.n;
    d->k = D_STRUCT;
    d->st = s;
    return;
  }
  if (!strcmp(what, "library")) {
    d->k = D_LIBRARY;
    d->lib = expect(p, T_STRING, "a library name, such as \"sqlite3\"")->s;
    return;
  }
  Token *conv = expect(p, T_VARID, "a calling convention, `ccall`");
  if (strcmp(conv->s, "ccall") != 0) cerror(conv->line, conv->col, "unknown calling convention `%s`; funk has `ccall`", conv->s);
  Token *ent = expect(p, T_STRING, "\"header.h name\"");
  ForeignDecl *f = NEW(ForeignDecl);
  f->line = t->line;
  f->col = t->col;
  if (!strcmp(what, "export")) { /* foreign export ccall "cname" name :: type (realtime.md, R4) */
    f->exported = true;
    f->cname = intern(xsprintf("%.*s", ent->len, ent->s));
    f->name = expect(p, T_VARID, "the funk function to export")->s;
    expect(p, T_DCOLON, NULL);
    f->sig = parse_sig(p);
    d->k = D_FOREIGN;
    d->ffi = f;
    return;
  }
  char *s = xsprintf("%.*s", ent->len, ent->s), *sp = strchr(s, ' ');
  if (!strcmp(s, "callback") || !strcmp(s, "queue")) { /* a trampoline: phase 3 of ffi.md */
    f->cb = s[0];
  } else {
    if (!sp || !sp[1])
      cerror(ent->line, ent->col, "a foreign import names a header and a C function or value: \"math.h sin\"");
    *sp = 0;
    f->header = intern(s);
    f->cname = intern(sp + 1); /* a value may be any C expression: "stddef.h sizeof(T)" */
  }
  f->name = expect(p, T_VARID, "the funk name of the import")->s;
  expect(p, T_DCOLON, NULL);
  f->sig = parse_sig(p);
  d->k = D_FOREIGN;
  d->ffi = f;
}

static void parse_decl(P *p, Decls *out, bool top) {
  Token *t = cur(p);
  Decl d = {0};
  d.line = t->line;
  d.col = t->col;
  if (top && foreign_start(t)) {
    parse_foreign(p, &d);
    VPUSH(*out, d);
    return;
  }
  if (t->k == T_DATA) {
    if (!top) cerror(t->line, t->col, "data declarations are only allowed at top level");
    d.k = D_DATA;
    d.data = parse_data(p);
    VPUSH(*out, d);
    derive(d.data, out);
    return;
  }
  if (t->k == T_CLASS || t->k == T_INSTANCE) {
    if (!top) cerror(t->line, t->col, "class and instance declarations are only allowed at top level");
    d.k = t->k == T_CLASS ? D_CLASS : D_INST;
    if (t->k == T_CLASS) d.cls = parse_class(p);
    else d.inst = parse_instance(p);
    VPUSH(*out, d);
    return;
  }
  if (is_fixity(p)) {
    if (!top) cerror(t->line, t->col, "fixity declarations are only allowed at top level");
    Names names = {0};
    parse_fixity(p, false, &names);
    d.k = D_FIXITY;
    d.names = names.a;
    d.nn = names.n;
    VPUSH(*out, d);
    return;
  }
  if (sig_start(p)) {
    Names names = {0};
    do {
      if (accept(p, T_LPAREN)) {
        VPUSH(names, take_op(p)->s);
        expect(p, T_RPAREN, NULL);
      } else if (qual_start(p, 0)) {
        VPUSH(names, take_qual(p));
      } else {
        VPUSH(names, expect(p, T_VARID, NULL)->s);
      }
    } while (accept(p, T_COMMA));
    expect(p, T_DCOLON, NULL);
    d.k = D_SIG;
    d.names = names.a;
    d.nn = names.n;
    d.sig = parse_sig(p);
    VPUSH(*out, d);
    return;
  }
  VEC(Pat *) ps = {0};
  if (t->k == T_LPAREN && op_len(p, 1) == 1 && ahead(p, 2)->k == T_RPAREN) {
    next(p);
    d.name = take_op(p)->s;
    next(p);
    while (apat_start(p)) VPUSH(ps, parse_apat(p));
  } else if (t->k == T_VARID && op_len(p, 1) && ahead(p, 1)->k != T_CONSYM) {
    VPUSH(ps, parse_apat(p)); /* x op y = ... */
    d.name = take_op(p)->s;
    VPUSH(ps, parse_lpat(p));
  } else if (t->k == T_VARID) {
    next(p);
    d.name = t->s;
    while (apat_start(p)) VPUSH(ps, parse_apat(p));
  } else if (qual_start(p, 0)) { /* Type.name args = ... */
    if (!top) cerror(t->line, t->col, "functions of a type must be defined at top level");
    d.name = take_qual(p);
    while (apat_start(p)) VPUSH(ps, parse_apat(p));
  } else {
    Pat *l = parse_lpat(p);
    if (op_len(p, 0) && cur(p)->k != T_CONSYM) { /* pattern op pattern = ... */
      VPUSH(ps, l);
      d.name = take_op(p)->s;
      VPUSH(ps, parse_lpat(p));
    } else {
      if (cur(p)->k == T_CONSYM) {
        Token *c = next(p);
        l = pcon(":", 2, (Pat *[]){l, parse_pat(p)}, c->line, c->col);
      }
      d.k = D_PATBIND;
      d.pat = l;
      d.rhs = parse_rhs(p, T_EQ, true);
      VPUSH(*out, d);
      return;
    }
  }
  d.k = D_CLAUSE;
  d.np = ps.n;
  d.pats = ps.a;
  d.rhs = parse_rhs(p, T_EQ, true);
  VPUSH(*out, d);
}

static Bind *mkbind(const char *name, Expr *e, int line, int col) {
  Bind *b = NEW(Bind);
  b->name = name;
  b->e = e;
  b->line = line;
  b->col = col;
  return b;
}

static void pat_vars(Pat *p, Pats *out) {
  if (p->k == P_VAR) VPUSH(*out, p);
  for (int i = 0; i < p->n; i++) pat_vars(p->args[i], out);
}

/* Group consecutive clauses into functions, split pattern bindings,
   attach signatures. */
static void build_binds(Decls *ds, Binds *out, Datas *datas) {
  int start = out->n;
  for (int i = 0; i < ds->n;) {
    Decl *d = &ds->a[i];
    if (d->k == D_DATA) {
      VPUSH(*datas, d->data);
      i++;
      continue;
    }
    if (d->k == D_SIG || d->k == D_FIXITY || d->k == D_CLASS || d->k == D_INST || d->k == D_FOREIGN || d->k == D_LIBRARY || d->k == D_STRUCT) {
      i++;
      continue;
    }
    if (d->k == D_PATBIND) {
      const char *tmp = fresh("p");
      VPUSH(*out, mkbind(tmp, rhs_expr(&d->rhs, d->line, d->col), d->line, d->col));
      Pats vs = {0};
      pat_vars(d->pat, &vs);
      for (int j = 0; j < vs.n; j++) {
        Expr **sc = xmalloc(sizeof *sc);
        sc[0] = mkvar(tmp, d->line, d->col);
        Pat **pp = xmalloc(sizeof *pp);
        pp[0] = pat_clone(d->pat);
        Alt **alts = xmalloc(sizeof *alts);
        alts[0] = mkalt(1, pp, simple_rhs(mkvar(vs.a[j]->name, vs.a[j]->line, vs.a[j]->col)));
        Expr *e = mkcase(1, sc, 1, alts, d->line, d->col);
        VPUSH(*out, mkbind(vs.a[j]->name, e, vs.a[j]->line, vs.a[j]->col));
      }
      i++;
      continue;
    }
    int j = i + 1;
    while (j < ds->n && ds->a[j].k == D_CLAUSE && ds->a[j].name == d->name) {
      if (ds->a[j].np != d->np)
        cerror(ds->a[j].line, ds->a[j].col, "clauses of `%s` have different numbers of arguments", d->name);
      j++;
    }
    int k = j - i;
    Expr *e;
    if (d->np == 0) {
      if (k > 1) cerror(ds->a[i + 1].line, ds->a[i + 1].col, "duplicate definition of `%s`", d->name);
      e = rhs_expr(&d->rhs, d->line, d->col);
    } else {
      Pat ***pats = xmalloc((size_t)k * sizeof *pats);
      Rhs *rhss = xmalloc((size_t)k * sizeof *rhss);
      for (int c = 0; c < k; c++) {
        pats[c] = ds->a[i + c].pats;
        rhss[c] = ds->a[i + c].rhs;
      }
      e = match_lambda(d->np, k, pats, rhss, d->line, d->col);
    }
    for (int b = start; b < out->n; b++)
      if (out->a[b]->name == d->name) cerror(d->line, d->col, "duplicate definition of `%s`", d->name);
    VPUSH(*out, mkbind(d->name, e, d->line, d->col));
    i = j;
  }
  for (int i = 0; i < ds->n; i++) {
    Decl *d = &ds->a[i];
    for (int n = 0; d->k == D_FIXITY && n < d->nn; n++) {
      bool found = false;
      for (int j = start; j < out->n; j++) found |= out->a[j]->name == d->names[n];
      if (!found) cerror(d->line, d->col, "fixity declaration for `%s` lacks a binding", d->names[n]);
    }
    if (d->k != D_SIG) continue;
    for (int n = 0; n < d->nn; n++) {
      Bind *b = NULL;
      for (int j = start; j < out->n; j++)
        if (out->a[j]->name == d->names[n]) b = out->a[j];
      if (!b) cerror(d->line, d->col, "type signature for `%s` lacks a binding", d->names[n]);
      if (b->sig) cerror(d->line, d->col, "duplicate type signature for `%s`", d->names[n]);
      b->sig = d->sig;
    }
  }
}

static void parse_decl_block(P *p, Binds *out) {
  Decls ds = {0};
  BLOCK(p, parse_decl(p, &ds, false));
  Datas none = {0};
  build_binds(&ds, out, &none);
}

/* ------------------------------------------------------------ entry points */

static P start(const char *src, size_t len) {
  P p = {0};
  p.t = lex(src, len, &p.n);
  return p;
}

/* A dotted module path: a varid and the field tokens after it, `a.b`;
   or Haskell's, `Data.List`, conids joined by `.` with no spaces. */
static const char *module_path(P *p, const char *what) {
  if (is(p, T_CONID)) {
    Token *t = next(p);
    const char *s = t->s;
    int end = t->col + (int)strlen(s);
    while (is(p, T_VARSYM) && !strcmp(cur(p)->s, ".") && cur(p)->col == end && ahead(p, 1)->k == T_CONID && ahead(p, 1)->col == end + 1) {
      next(p);
      Token *c = next(p);
      s = intern(xsprintf("%s.%s", s, c->s));
      end = c->col + (int)strlen(c->s);
    }
    return s;
  }
  const char *s = expect(p, T_VARID, what)->s;
  while (is(p, T_FIELD)) s = intern(xsprintf("%s.%s", s, next(p)->s));
  return s;
}

static bool path_start(P *p, int off) { return ahead(p, off)->k == T_VARID || ahead(p, off)->k == T_CONID; }

static bool word(P *p, int off, const char *w) { return ahead(p, off)->k == T_VARID && !strcmp(ahead(p, off)->s, w); }

/* (x, (op), T, T(..), T(A, b), ...) */
static ListItems parse_items(P *p) {
  ListItems its = {0};
  expect(p, T_LPAREN, NULL);
  while (!is(p, T_RPAREN)) {
    Token *t = cur(p);
    ListItem it = {.line = t->line, .col = t->col};
    if (accept(p, T_LPAREN)) {
      if (!is(p, T_VARSYM) && !is(p, T_CONSYM)) unexpected(p, "an operator");
      it.name = next(p)->s;
      expect(p, T_RPAREN, NULL);
    } else if (is(p, T_VARID)) {
      it.name = next(p)->s;
    } else {
      it.name = expect(p, T_CONID, "a name")->s;
      if (accept(p, T_LPAREN)) {
        if (accept(p, T_DOTDOT)) {
          it.all = true;
        } else {
          VEC(const char *) subs = {0};
          while (!is(p, T_RPAREN)) {
            VPUSH(subs, expect(p, T_CONID, "a constructor")->s);
            if (!accept(p, T_COMMA)) break;
          }
          it.subs = subs.a;
          it.nsubs = subs.n;
        }
        expect(p, T_RPAREN, NULL);
      }
    }
    VPUSH(its, it);
    if (!accept(p, T_COMMA)) break;
  }
  expect(p, T_RPAREN, NULL);
  return its;
}

/* `module name where` and the `import` lines at the top of a file
   (modules.md). The words are reserved only there. */
static void parse_header(P *p, Module *m) {
  while (accept(p, T_SEMI)) {}
  if (word(p, 0, "module") && path_start(p, 1)) {
    Token *t = next(p);
    m->line = t->line;
    m->col = t->col;
    m->name = module_path(p, "a module name");
    if (is(p, T_LPAREN)) {
      m->exports_listed = true;
      m->exports = parse_items(p);
    }
    expect(p, T_WHERE, "`where` after the module name");
    expect(p, T_VLBRACE, NULL);
    if (!accept(p, T_VRBRACE)) cerror(t->line, t->col, "a module's declarations start at column 1, as the `module` line does");
  }
  VEC(Import) ims = {0};
  for (;;) {
    while (accept(p, T_SEMI)) {}
    if (!word(p, 0, "import") || !path_start(p, 1)) break;
    Token *t = next(p);
    Import im = {.line = t->line, .col = t->col};
    if (word(p, 0, "qualified") && path_start(p, 1)) next(p); /* Haskell's; funk's imports are qualified anyway */
    im.base = is(p, T_CONID);
    im.path = module_path(p, "a module name");
    const char *dot = strrchr(im.path, '.');
    im.qual = im.base ? im.path : dot ? intern(dot + 1) : im.path; /* Haskell qualifies by the whole path */
    if (word(p, 0, "as")) {
      next(p);
      if (!is(p, T_VARID) && !is(p, T_CONID)) unexpected(p, "a name after `as`");
      im.qual = next(p)->s;
    }
    if (word(p, 0, "hiding")) cerror(cur(p)->line, cur(p)->col, "`hiding` is not supported; list the names to import instead");
    if (is(p, T_LPAREN)) {
      im.listed = true;
      im.items = parse_items(p);
    }
    for (int i = 0; i < ims.n; i++)
      if (ims.a[i].qual == im.qual) cerror(t->line, t->col, "`%s` already names an import, at line %d", im.qual, ims.a[i].line);
    if (!is(p, T_SEMI) && !is(p, T_EOF)) unexpected(p, "a new line after the import");
    VPUSH(ims, im);
  }
  m->imports = ims.a;
  m->nimports = ims.n;
}

/* Tokens from i that spell qualifier q with no spaces (`geometry`,
   `Data.Char`): their number, or 0; *end is the column after them. */
static int qual_len(P *p, int i, const char *q, int *end) {
  SB b = {0};
  sb_puts(&b, "");
  for (int j = i; j < p->n; j++) {
    Token *t = &p->t[j];
    if (j > i && (t->line != p->t[i].line || t->col != *end)) break;
    if ((j - i) % 2 == 0 ? t->k != T_VARID && t->k != T_CONID : t->k != T_VARSYM || strcmp(t->s, ".")) break;
    sb_puts(&b, t->s);
    *end = t->col + (int)strlen(t->s);
    if ((j - i) % 2 == 0 && !strcmp(b.s, q)) {
      xfree(b.s);
      return j - i + 1;
    }
    if (strncmp(q, b.s, b.n)) break;
  }
  xfree(b.s);
  return 0;
}

/* q.x, q.X and q.op with no spaces, q an import's qualifier: one token
   named `q.x`, `q.X` or `q.op`. Elsewhere `f.X` stays composition. */
static void join_qualified(P *p, Module *m) {
  if (!m->nimports) return;
  int o = p->pos;
  for (int i = p->pos; i < p->n; i++) {
    Token t = p->t[i];
    int n = 0, end = 0;
    const char *q = NULL;
    for (int k = 0; k < m->nimports; k++) { /* the longest: Data.Char over Data */
      int e, l = qual_len(p, i, m->imports[k].qual, &e);
      if (l > n) n = l, end = e, q = m->imports[k].qual;
    }
    Token *d = &p->t[i + n < p->n ? i + n : p->n - 1];
    bool at = n && d->line == t.line && d->col == end;
    if (at && d->k == T_FIELD) {
      t.k = T_VARID;
      t.s = intern(xsprintf("%s.%s", q, d->s));
      i += n;
    } else if (at && d->k == T_VARSYM && d->s[0] == '.' && d->s[1]) {
      t.k = d->s[1] == ':' ? T_CONSYM : T_VARSYM;
      t.s = intern(xsprintf("%s%s", q, d->s));
      i += n;
    } else if (at && d->k == T_VARSYM && !strcmp(d->s, ".") && i + n + 1 < p->n && p->t[i + n + 1].k == T_CONID &&
               p->t[i + n + 1].line == t.line && p->t[i + n + 1].col == end + 1) {
      t.k = T_CONID;
      t.s = intern(xsprintf("%s.%s", q, p->t[i + n + 1].s));
      i += n + 1;
    }
    p->t[o++] = t;
  }
  p->n = o;
}

/* The header first, so that the driver can compile the imports, whose
   fixities the body's operators need, before parse_finish. */
Module *parse_begin(const char *src, size_t len) {
  P *p = NEW(P);
  *p = start(src, len);
  Module *m = NEW(Module);
  parse_header(p, m);
  m->pending = p;
  return m;
}

Module *parse_module(const char *src, size_t len) {
  Module *m = parse_begin(src, len);
  parse_finish(m);
  return m;
}

void parse_finish(Module *m) {
  P p = *(P *)m->pending;
  m->pending = NULL;
  prescan_fixities(p);
  join_qualified(&p, m);
  Decls ds = {0};
  while (!is(&p, T_EOF)) {
    if (accept(&p, T_SEMI)) continue;
    parse_decl(&p, &ds, true);
    if (!is(&p, T_SEMI) && !is(&p, T_EOF)) unexpected(&p, "a new declaration at column 1");
  }
  Binds bs = {0};
  Datas datas = {0};
  build_binds(&ds, &bs, &datas);
  m->binds = bs.a;
  m->nb = bs.n;
  m->datas = datas.a;
  m->nd = datas.n;
  VEC(ClassDecl *) cs = {0};
  VEC(InstDecl *) is = {0};
  VEC(ForeignDecl *) fs = {0};
  VEC(const char *) libs = {0};
  VEC(StructDecl *) sts = {0};
  for (int i = 0; i < ds.n; i++) {
    if (ds.a[i].k == D_STRUCT) VPUSH(sts, ds.a[i].st);
    if (ds.a[i].k == D_CLASS) VPUSH(cs, ds.a[i].cls);
    if (ds.a[i].k == D_INST) VPUSH(is, ds.a[i].inst);
    if (ds.a[i].k == D_FOREIGN) VPUSH(fs, ds.a[i].ffi);
    if (ds.a[i].k == D_LIBRARY) VPUSH(libs, ds.a[i].lib);
  }
  m->foreigns = fs.a;
  m->nforeign = fs.n;
  m->libs = libs.a;
  m->nlibs = libs.n;
  m->structs = sts.a;
  m->nstructs = sts.n;
  m->classes = cs.a;
  m->ncl = cs.n;
  m->insts = is.a;
  m->ni = is.n;
}

Expr *parse_expr_text(const char *src, size_t len, Import *ims, int nims) {
  P p = start(src, len);
  Module scope = {.imports = ims, .nimports = nims}; /* the REPL's imports */
  join_qualified(&p, &scope);
  Expr *e = parse_expr(&p);
  while (accept(&p, T_SEMI)) {}
  expect(&p, T_EOF, "end of expression");
  return e;
}

Sig *parse_sig_text(const char *src) {
  P p = start(src, strlen(src));
  Sig *s = parse_sig(&p);
  expect(&p, T_EOF, NULL);
  return s;
}

/* REPL: input is a declaration if it starts with `data` or has `=` at
   bracket depth 0 before any expression keyword. */
bool is_decl_text(const char *src, size_t len) {
  int n;
  Token *t = lex(src, len, &n);
  if (t[0].k == T_DATA || t[0].k == T_CLASS || t[0].k == T_INSTANCE || t[0].k == T_INFIXL || t[0].k == T_INFIXR || t[0].k == T_INFIX || (n > 1 && foreign_start(t)) || sig_start(&(P){t, n, 0}))
    return true;
  int depth = 0;
  for (int i = 0; i < n; i++) {
    switch (t[i].k) {
    case T_LPAREN: case T_LBRACKET: case T_LBRACE: depth++; break;
    case T_RPAREN: case T_RBRACKET: case T_RBRACE: depth--; break;
    case T_EQ: if (depth == 0) return true; break;
    case T_LET: case T_LAMBDA: case T_CASE: case T_IF: case T_OF: case T_DO: return false;
    default: break;
    }
  }
  return false;
}
