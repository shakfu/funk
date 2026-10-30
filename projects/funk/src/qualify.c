/* qualify.c -- module scope (modules.md). Before a module is checked, its
   top-level names become `prefix.name`, as do its references to them that
   no local shadows. `q.name`, for an import's qualifier q, and a name an
   import list brings in become that module's `prefix.name`, if it exports
   the name. Later passes then see one flat set of unique names. Types and
   constructors have no local scope; values do. Names starting with `$`
   are generated, already unique, and kept. */
#include <ctype.h>

#include "funk.h"


typedef VEC(const char *) Names;

/* A name an import list brings in, unqualified. */
typedef struct { const char *name, *target; int ns; ImportInfo *from; } Unq;

typedef struct {
  const char *prefix; /* NULL: the program, whose own names stay */
  ImportInfo *imps;
  int nimps;
  Names values, types, cons, classes; /* the module's own */
  VEC(Unq) unq;
  Names scope; /* locals in scope */
} Q;

static bool has(Names *s, const char *x) {
  for (int i = s->n; i-- > 0;)
    if (s->a[i] == x) return true;
  return false;
}

static const char *const ns_word[] = {"", "type ", "constructor ", "class "};

static Export *exported(Exports *ex, int ns, const char *x) {
  for (int i = 0; i < ex->n; i++)
    if (ex->a[i].ns == ns && ex->a[i].name == x) return &ex->a[i];
  return NULL;
}

static ImportInfo *qualifier(Q *q, const char *x) {
  for (int i = 0; i < q->nimps; i++)
    if (q->imps[i].im->qual == x) return &q->imps[i];
  return NULL;
}

static const char *with_prefix(const char *prefix, const char *x) { return intern(xsprintf("%s.%s", prefix, x)); }

static void need_export(ImportInfo *in, int ns, const char *x, int line, int col) {
  if (!in->ex) { /* a Haskell base module: the prelude */
    if (!in_prelude(ns, x)) cerror(line, col, "%s`%s` is not in funk's prelude, which stands in for `%s`", ns_word[ns], x, in->im->path);
    return;
  }
  if (!exported(in->ex, ns, x)) cerror(line, col, "module `%s` does not export %s`%s`", in->im->path, ns_word[ns], x);
}

/* q.rest for an import's qualifier q: its module's prefix.rest, or
   rest for a base module. q may hold dots (Data.Char), so the longest
   qualifier that x starts with wins. */
static const char *imported(Q *q, const char *x, int ns, int line, int col) {
  ImportInfo *in = NULL;
  size_t n = 0;
  for (int i = 0; i < q->nimps; i++) {
    size_t l = strlen(q->imps[i].im->qual);
    if (l > n && !strncmp(x, q->imps[i].im->qual, l) && x[l] == '.' && x[l + 1]) in = &q->imps[i], n = l;
  }
  if (!in) return NULL;
  const char *rest = intern(x + n + 1);
  need_export(in, ns, rest, line, col);
  return in->prefix ? with_prefix(in->prefix, rest) : rest;
}

static Unq *unqualified(Q *q, int ns, const char *x) {
  for (int i = 0; i < q->unq.n; i++)
    if (q->unq.a[i].ns == ns && q->unq.a[i].name == x) return &q->unq.a[i];
  return NULL;
}

static const char *own(Q *q, Names *s, const char *x) { return q->prefix && x[0] != '$' && has(s, x) ? with_prefix(q->prefix, x) : x; }

static const char *ren(Q *q, int ns, Names *s, const char *x, int line, int col) {
  const char *i = imported(q, x, ns, line, col);
  if (i) return i;
  if (has(s, x)) return own(q, s, x);
  Unq *u = unqualified(q, ns, x);
  return u ? u->target : x;
}
static const char *ren_type(Q *q, const char *x, int line, int col) { return ren(q, NS_TYPE, &q->types, x, line, col); }
static const char *ren_con(Q *q, const char *x, int line, int col) { return ren(q, NS_CON, &q->cons, x, line, col); }
static const char *ren_class(Q *q, const char *x, int line, int col) { return ren(q, NS_CLASS, &q->classes, x, line, col); }

static const char *ren_value(Q *q, const char *x, int line, int col) {
  if (has(&q->scope, x)) return x;
  const char *i = imported(q, x, NS_VALUE, line, col);
  if (i) return i;
  if (qualifier(q, x)) cerror(line, col, "`%s` names an imported module; use `%s.name`", x, x);
  const char *dot = ns_dot(x); /* T.member, T this module's or imported unqualified */
  if (dot) {
    const char *tn = intern_n(x, (size_t)(dot - x));
    if (has(&q->types, tn)) return q->prefix ? with_prefix(q->prefix, x) : x;
    Unq *u = unqualified(q, NS_TYPE, tn);
    if (u) {
      need_export(u->from, NS_VALUE, x, line, col);
      return with_prefix(u->from->prefix, x);
    }
  }
  if (has(&q->values, x)) return own(q, &q->values, x);
  Unq *u = unqualified(q, NS_VALUE, x);
  return u ? u->target : x;
}

static void bind_local(Q *q, const char *x, int line, int col) {
  if (qualifier(q, x)) cerror(line, col, "`%s` names an imported module, so it cannot also name a variable", x);
  VPUSH(q->scope, x);
}

static void q_type(Q *q, TypeExpr *t) {
  if (!t) return;
  if (t->k == TE_CON) t->name = ren_type(q, t->name, t->line, t->col);
  for (int i = 0; i < t->n; i++) q_type(q, t->args[i]);
}

static void q_sig(Q *q, Sig *s) {
  if (!s) return;
  for (int i = 0; i < s->nctx; i++) s->ctx_cls[i] = ren_class(q, s->ctx_cls[i], s->line, s->col);
  q_type(q, s->t);
}

static void q_pat(Q *q, Pat *p) {
  if (p->k == P_CON) p->name = ren_con(q, p->name, p->line, p->col);
  if (p->k == P_VAR) bind_local(q, p->name, p->line, p->col);
  for (int i = 0; i < p->n; i++) q_pat(q, p->args[i]);
}

static void q_expr(Q *q, Expr *e);

static void q_bind(Q *q, Bind *b) {
  q_sig(q, b->sig);
  q_expr(q, b->e);
}

/* Groups whose names are in scope in each other and in what follows. */
static void q_groups(Q *q, Group *g, int ng) {
  for (int i = 0; i < ng; i++)
    for (int j = 0; j < g[i].n; j++) bind_local(q, g[i].b[j]->name, g[i].b[j]->line, g[i].b[j]->col);
  for (int i = 0; i < ng; i++)
    for (int j = 0; j < g[i].n; j++) q_bind(q, g[i].b[j]);
}

static void q_expr(Q *q, Expr *e) {
  if (!e) return;
  int mark = q->scope.n;
  switch (e->k) {
  case E_VAR: e->name = ren_value(q, e->name, e->line, e->col); break;
  case E_CON: e->name = ren_con(q, e->name, e->line, e->col); break;
  case E_APP:
  case E_RECORD:
    q_expr(q, e->fn);
    for (int i = 0; i < e->n; i++) q_expr(q, e->args[i]);
    break;
  case E_LAM:
    for (int i = 0; i < e->lam->n; i++) bind_local(q, e->lam->params[i], e->lam->line, e->lam->col);
    q_expr(q, e->lam->body);
    break;
  case E_LET:
    q_groups(q, &e->g, 1);
    q_expr(q, e->body);
    break;
  case E_IF:
    q_expr(q, e->cond);
    q_expr(q, e->then_);
    q_expr(q, e->else_);
    break;
  case E_CASE:
    for (int i = 0; i < e->ns; i++) q_expr(q, e->scruts[i]);
    for (int a = 0; a < e->na; a++) {
      Alt *alt = e->alts[a];
      int am = q->scope.n;
      for (int i = 0; i < alt->np; i++) q_pat(q, alt->pats[i]);
      q_groups(q, alt->rhs.wg, alt->rhs.nwg);
      for (int i = 0; i < alt->rhs.ng; i++) {
        q_expr(q, alt->rhs.guards[i]);
        q_expr(q, alt->rhs.bodies[i]);
      }
      q->scope.n = am;
    }
    break;
  case E_ANN:
    q_expr(q, e->body);
    q_sig(q, e->sig);
    break;
  case E_FIELD: q_expr(q, e->body); break;
  default: break;
  }
  q->scope.n = mark;
}

/* ------------------------------------------------------------ the module's names */

/* A module's own name: not the prelude's, not an import's qualifier, not
   one an import list brings in. */
static void define(Q *q, Names *s, int ns, const char *x, int line, int col) {
  if (x[0] == '$') return;
  if (q->prefix && in_prelude(ns, x)) cerror(line, col, "%s`%s` is defined in the prelude and cannot be redefined; choose another name", ns_word[ns], x);
  if (ns == NS_VALUE && qualifier(q, x)) cerror(line, col, "`%s` names an imported module, so it cannot also name a definition", x);
  Unq *u = unqualified(q, ns, x);
  if (u) cerror(line, col, "%s`%s` is imported from `%s`; choose another name", ns_word[ns], x, u->from->im->path);
  VPUSH(*s, x);
}

static void add(Exports *ex, int ns, const char *name, const char *parent) {
  if (!exported(ex, ns, name)) VPUSH(*ex, ((Export){name, parent, ns}));
}

/* Everything m defines: its export table when it has no list. */
static Exports *all_exports(Module *m) {
  Exports *ex = NEW(Exports);
  for (int i = 0; i < m->nb; i++) {
    const char *x = m->binds[i]->name, *dot = ns_dot(x); /* T.f goes with T */
    if (x[0] != '$') add(ex, NS_VALUE, x, dot ? intern_n(x, (size_t)(dot - x)) : NULL);
  }
  for (int i = 0; i < m->nforeign; i++)
    if (!m->foreigns[i]->exported) add(ex, NS_VALUE, m->foreigns[i]->name, NULL);
  for (int i = 0; i < m->ncl; i++) {
    ClassDecl *c = m->classes[i];
    add(ex, NS_CLASS, c->name, NULL);
    for (int j = 0; j < c->nsig; j++) add(ex, NS_VALUE, c->names[j], c->name);
  }
  for (int i = 0; i < m->nd; i++) {
    DataDecl *d = m->datas[i];
    add(ex, NS_TYPE, d->name, NULL);
    for (int j = 0; j < d->nc; j++) {
      add(ex, NS_CON, d->cons[j].name, d->name);
      for (int k = 0; k < d->cons[j].nfields && d->cons[j].labels; k++)
        add(ex, NS_VALUE, with_prefix(d->name, d->cons[j].labels[k]), d->name); /* T.f */
    }
  }
  for (int i = 0; i < m->nstructs; i++) {
    StructDecl *s = m->structs[i];
    add(ex, NS_TYPE, s->name, NULL);
    for (int j = 0; j < s->nf; j++) add(ex, NS_VALUE, with_prefix(s->name, s->fields[j].name), s->name);
  }
  return ex;
}

static bool is_field(Module *m, const char *tn, const char *x) {
  for (int i = 0; i < m->nd; i++)
    for (int j = 0; m->datas[i]->name == tn && j < m->datas[i]->nc; j++)
      for (int k = 0; k < m->datas[i]->cons[j].nfields && m->datas[i]->cons[j].labels; k++)
        if (m->datas[i]->cons[j].labels[k] == x) return true;
  for (int i = 0; i < m->nstructs; i++)
    for (int j = 0; m->structs[i]->name == tn && j < m->structs[i]->nf; j++)
      if (m->structs[i]->fields[j].name == x) return true;
  return false;
}

/* The module's export list against everything it defines. A type's
   namespace functions go with it; its constructors and fields only with
   T(..), or the constructors named in T(A, B). */
static Exports *list_exports(Q *q, Module *m, Exports *all) {
  Exports *ex = NEW(Exports);
  for (int i = 0; i < m->exports.n; i++) {
    ListItem *it = &m->exports.a[i];
    const char *x = it->name;
    if (!isupper((unsigned char)x[0])) {
      if (!exported(all, NS_VALUE, x) || strchr(x, '.')) cerror(it->line, it->col, "the export list names `%s`, which this module does not define", x);
      add(ex, NS_VALUE, x, NULL);
      continue;
    }
    bool type = exported(all, NS_TYPE, x), cls = exported(all, NS_CLASS, x);
    if (!type && !cls) cerror(it->line, it->col, "the export list names `%s`, which this module does not define as a type or class", x);
    add(ex, type ? NS_TYPE : NS_CLASS, x, NULL);
    for (int k = 0; k < all->n; k++) {
      Export *e = &all->a[k];
      if (e->parent != x) continue;
      const char *dot = e->ns == NS_VALUE && type ? ns_dot(e->name) : NULL;
      bool field = dot && is_field(m, x, intern(dot + 1));
      bool listed = false;
      for (int j = 0; j < it->nsubs; j++) listed |= it->subs[j] == e->name;
      if (it->all || listed || (type && e->ns == NS_VALUE && !field)) add(ex, e->ns, e->name, e->parent);
    }
    for (int j = 0; j < it->nsubs; j++) {
      Export *e = exported(all, type ? NS_CON : NS_VALUE, it->subs[j]);
      if (!e || e->parent != x) cerror(it->line, it->col, "`%s` has no %s `%s`", x, type ? "constructor" : "method", it->subs[j]);
    }
  }
  /* Fields reach by type, not by name (`v.f` on a value an exported
     function returns), so check.c is told which types keep them inside. */
  for (int i = 0; i < q->types.n; i++) {
    const char *x = q->types.a[i];
    bool open = false;
    for (int j = 0; j < m->exports.n; j++) open |= m->exports.a[j].name == x && m->exports.a[j].all;
    if (!open) VPUSH(CS->sealed_types, with_prefix(q->prefix, x));
    if (!exported(ex, NS_TYPE, x)) VPUSH(CS->hidden_types, with_prefix(q->prefix, x));
  }
  return ex;
}

/* An import list's names, unqualified in this file. */
static void import_list(Q *q, ImportInfo *in) {
  Exports *ex = in->ex;
  for (int i = 0; ex && i < in->im->items.n; i++) {
    ListItem *it = &in->im->items.a[i];
    Unq us[64];
    int nu = 0;
    const char *x = it->name;
    if (!isupper((unsigned char)x[0])) {
      need_export(in, NS_VALUE, x, it->line, it->col);
      us[nu++] = (Unq){x, NULL, NS_VALUE, in};
    } else {
      int ns = exported(ex, NS_TYPE, x) ? NS_TYPE : NS_CLASS;
      need_export(in, ns, x, it->line, it->col);
      us[nu++] = (Unq){x, NULL, ns, in};
      int sub = ns == NS_TYPE ? NS_CON : NS_VALUE;
      for (int k = 0; k < ex->n && nu < 64; k++)
        if (it->all && ex->a[k].parent == x && ex->a[k].ns == sub) us[nu++] = (Unq){ex->a[k].name, NULL, sub, in};
      for (int j = 0; j < it->nsubs && nu < 64; j++) {
        Export *e = exported(ex, sub, it->subs[j]);
        if (!e || e->parent != x) cerror(it->line, it->col, "module `%s` does not export %s`%s` of `%s`", in->im->path, sub == NS_CON ? "constructor " : "method ", it->subs[j], x);
        us[nu++] = (Unq){it->subs[j], NULL, sub, in};
      }
    }
    for (int k = 0; k < nu; k++) {
      Unq *prev = unqualified(q, us[k].ns, us[k].name);
      if (prev && prev->from != in)
        cerror(it->line, it->col, "%s`%s` is imported from both `%s` and `%s`", ns_word[us[k].ns], us[k].name, prev->from->im->path, in->im->path);
      us[k].target = with_prefix(in->prefix, us[k].name);
      if (!prev) VPUSH(q->unq, us[k]);
    }
  }
}

Exports *qualify_module(Module *m, const char *prefix, ImportInfo *imps, int nimps) {
  Q q = {.prefix = prefix, .imps = imps, .nimps = nimps};
  for (int i = 0; i < nimps; i++) {
    if (imps[i].im->listed && imps[i].ex) import_list(&q, &imps[i]);
    for (int j = 0; imps[i].im->listed && !imps[i].ex && j < imps[i].im->items.n; j++) { /* a base module's: in the prelude already */
      ListItem *it = &imps[i].im->items.a[j];
      bool upper = isupper((unsigned char)it->name[0]);
      int ns = !upper ? NS_VALUE : in_prelude(NS_TYPE, it->name) ? NS_TYPE : NS_CLASS;
      need_export(&imps[i], ns, it->name, it->line, it->col);
      for (int k = 0; k < it->nsubs; k++) need_export(&imps[i], ns == NS_TYPE ? NS_CON : NS_VALUE, it->subs[k], it->line, it->col);
    }
  }
  for (int i = 0; i < m->nb; i++) define(&q, &q.values, NS_VALUE, m->binds[i]->name, m->binds[i]->line, m->binds[i]->col);
  for (int i = 0; i < m->nforeign; i++)
    if (!m->foreigns[i]->exported) define(&q, &q.values, NS_VALUE, m->foreigns[i]->name, m->foreigns[i]->line, m->foreigns[i]->col);
  for (int i = 0; i < m->ncl; i++) {
    ClassDecl *c = m->classes[i];
    define(&q, &q.classes, NS_CLASS, c->name, c->line, c->col);
    for (int j = 0; j < c->nsig; j++) define(&q, &q.values, NS_VALUE, c->names[j], c->sigs[j]->line, c->sigs[j]->col);
  }
  for (int i = 0; i < m->nd; i++) {
    DataDecl *d = m->datas[i];
    define(&q, &q.types, NS_TYPE, d->name, d->line, d->col);
    for (int j = 0; j < d->nc; j++) define(&q, &q.cons, NS_CON, d->cons[j].name, d->cons[j].line, d->cons[j].col);
  }
  for (int i = 0; i < m->nstructs; i++) define(&q, &q.types, NS_TYPE, m->structs[i]->name, m->structs[i]->line, m->structs[i]->col);
  Exports *all = all_exports(m), *ex = m->exports_listed && prefix ? list_exports(&q, m, all) : all; /* the program's `module Main (main)`: ignored */

  for (int i = 0; i < m->nb; i++) {
    Bind *b = m->binds[i];
    q_bind(&q, b);
    b->name = ren_value(&q, b->name, b->line, b->col);
  }
  for (int i = 0; i < m->nforeign; i++) {
    ForeignDecl *f = m->foreigns[i];
    q_sig(&q, f->sig);
    f->name = f->exported ? ren_value(&q, f->name, f->line, f->col) : own(&q, &q.values, f->name);
  }
  for (int i = 0; i < m->ncl; i++) {
    ClassDecl *c = m->classes[i];
    c->name = ren_class(&q, c->name, c->line, c->col);
    for (int j = 0; j < c->nsup; j++) c->sups[j] = ren_class(&q, c->sups[j], c->line, c->col);
    for (int j = 0; j < c->nsig; j++) {
      q_sig(&q, c->sigs[j]);
      c->names[j] = own(&q, &q.values, c->names[j]);
    }
    for (int j = 0; j < c->ndef; j++) {
      q_bind(&q, c->defs[j]);
      c->defs[j]->name = own(&q, &q.values, c->defs[j]->name);
    }
  }
  for (int i = 0; i < m->ni; i++) {
    InstDecl *in = m->insts[i];
    const char *src = in->cls;
    in->cls = ren_class(&q, in->cls, in->line, in->col);
    ImportInfo *from = NULL; /* an imported class: its methods must be exported */
    const char *dot = strchr(src, '.');
    if (dot) from = qualifier(&q, intern_n(src, (size_t)(dot - src)));
    else if (!has(&q.classes, src) && unqualified(&q, NS_CLASS, src)) from = unqualified(&q, NS_CLASS, src)->from;
    q_type(&q, in->head);
    q_sig(&q, in->ctx);
    dot = strrchr(in->cls, '.'); /* a module's class: its methods have its prefix */
    const char *pre = dot ? intern_n(in->cls, (size_t)(dot - in->cls)) : NULL;
    for (int j = 0; j < in->nb; j++) {
      if (from) need_export(from, NS_VALUE, in->binds[j]->name, in->binds[j]->line, in->binds[j]->col);
      q_bind(&q, in->binds[j]);
      if (pre) in->binds[j]->name = with_prefix(pre, in->binds[j]->name);
    }
  }
  for (int i = 0; i < m->nd; i++) {
    DataDecl *d = m->datas[i];
    d->name = ren_type(&q, d->name, d->line, d->col);
    for (int j = 0; j < d->nderiv; j++) d->derivs[j] = ren_class(&q, d->derivs[j], d->derive_line, d->derive_col);
    for (int j = 0; j < d->nc; j++) {
      d->cons[j].name = ren_con(&q, d->cons[j].name, d->cons[j].line, d->cons[j].col);
      for (int k = 0; k < d->cons[j].nfields; k++) q_type(&q, d->cons[j].fields[k]);
    }
  }
  for (int i = 0; i < m->nstructs; i++) {
    StructDecl *s = m->structs[i];
    s->name = ren_type(&q, s->name, s->line, s->col);
    for (int j = 0; j < s->nf; j++) q_type(&q, s->fields[j].type);
  }
  free(q.values.a);
  free(q.types.a);
  free(q.cons.a);
  free(q.classes.a);
  free(q.unq.a);
  free(q.scope.a);
  return ex;
}
