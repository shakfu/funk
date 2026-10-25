/* parse.c -- recursive-descent parser. Desugars as it goes:
     multi-clause functions   -> \fresh.. -> case fresh.. of clauses
     x op y                   -> op x y   (&& and || become `if`)
     (op e), (e op)           -> flip op e, op e
     [a, b], [a..b], "str"    -> cons chains, enumFromTo
     (p1, p2) = e             -> one binding per pattern variable
   Record syntax (p.x, C { f = e }, r { f = e }) is kept as E_FIELD and
   E_RECORD; check.c lowers it once constructor fields are known. */
#include "funk.h"

typedef struct { Token *t; int n, pos; } P;

enum { D_CLAUSE, D_PATBIND, D_SIG, D_DATA, D_FIXITY };
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

static int fresh_counter;
static const char *fresh(const char *prefix) { return intern(xsprintf("$%s%d", prefix, fresh_counter++)); }

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
    {">>>", 1, 'r'}, {"$", 0, 'r'}};

/* Declared fixities, as prec * 3 + index in "lrn". Global so that later
   REPL inputs see them; main.c rolls them back with a failed input. */
static Map user_fixities;

static Fixity fixity(const char *op) {
  int v = map_get(&user_fixities, op);
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
    if (reg) map_put(&user_fixities, op->s, prec * 3 + assoc);
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
  free(names.a);
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
  memcpy(e->args, args, (size_t)n * sizeof *args);
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

static Expr *binop(Token *op, Expr *l, Expr *r) {
  if (op->s == intern("&&")) return mkif(l, r, mkcon("False", op->line, op->col));
  if (op->s == intern("||")) return mkif(l, mkcon("True", op->line, op->col), r);
  return app2(op_expr(op), l, r);
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
  e->name = cur_file; /* for match-failure messages */
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
  if (!accept(p, T_ARROW)) return a;
  TypeExpr *f = mkte(TE_FUN, t);
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
    for (int i = 0; i < n; i++) {
      TypeExpr *c = cs[i];
      if (c->k != TE_CON || c->n != 1 || c->args[0]->k != TE_VAR)
        cerror(c->line, c->col, "constraint must have the form `Num a`");
      if (strcmp(c->name, "Num") != 0)
        cerror(c->line, c->col, "unknown class `%s` (only Num is supported)", c->name);
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

static Pat *string_pat(Token *t) {
  Pat *pt = pcon("[]", 0, NULL, t->line, t->col);
  for (int i = t->len; i-- > 0;) {
    Pat *c = mkpat(P_CHAR, t->line, t->col);
    c->ival = (unsigned char)t->s[i];
    pt = pcon(":", 2, (Pat *[]){c, pt}, t->line, t->col);
  }
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

/* Items of an explicit or layout block, separated by ';'. */
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
  if (accept(p, T_DERIVING)) { /* Eq, Ord and Show are structural already */
    if (accept(p, T_LPAREN)) {
      if (!is(p, T_RPAREN)) {
        do expect(p, T_CONID, "a class name");
        while (accept(p, T_COMMA));
      }
      expect(p, T_RPAREN, NULL);
    } else {
      expect(p, T_CONID, "a class name");
    }
  }
  return d;
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

static void parse_decl(P *p, Decls *out, bool top) {
  Token *t = cur(p);
  Decl d = {0};
  d.line = t->line;
  d.col = t->col;
  if (t->k == T_DATA) {
    if (!top) cerror(t->line, t->col, "data declarations are only allowed at top level");
    d.k = D_DATA;
    d.data = parse_data(p);
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
    if (d->k == D_SIG || d->k == D_FIXITY) { i++; continue; }
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

Module *parse_module(const char *src, size_t len) {
  P p = start(src, len);
  prescan_fixities(p);
  Decls ds = {0};
  while (!is(&p, T_EOF)) {
    if (accept(&p, T_SEMI)) continue;
    parse_decl(&p, &ds, true);
    if (!is(&p, T_SEMI) && !is(&p, T_EOF)) unexpected(&p, "a new declaration at column 1");
  }
  Module *m = NEW(Module);
  Binds bs = {0};
  Datas datas = {0};
  build_binds(&ds, &bs, &datas);
  m->binds = bs.a;
  m->nb = bs.n;
  m->datas = datas.a;
  m->nd = datas.n;
  return m;
}

Expr *parse_expr_text(const char *src, size_t len) {
  P p = start(src, len);
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
  if (t[0].k == T_DATA || t[0].k == T_INFIXL || t[0].k == T_INFIXR || t[0].k == T_INFIX || sig_start(&(P){t, n, 0}))
    return true;
  int depth = 0;
  for (int i = 0; i < n; i++) {
    switch (t[i].k) {
    case T_LPAREN: case T_LBRACKET: case T_LBRACE: depth++; break;
    case T_RPAREN: case T_RBRACKET: case T_RBRACE: depth--; break;
    case T_EQ: if (depth == 0) return true; break;
    case T_LET: case T_LAMBDA: case T_CASE: case T_IF: case T_OF: return false;
    default: break;
    }
  }
  return false;
}
