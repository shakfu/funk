/* funk.h -- compiler data structures shared by all passes.
   Pipeline: lex+layout -> parse (desugar) -> check (SCC, inference)
             -> resolve (closure conversion) -> interp | cgen */
#ifndef FUNK_H
#define FUNK_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

/* ------------------------------------------------------------------ util */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xsprintf(const char *fmt, ...);
const char *intern(const char *s);
const char *intern_n(const char *s, size_t n);
char *read_file(const char *path, size_t *len);
#define NEW(T) ((T *)xcalloc(1, sizeof(T)))

#define VEC(T) struct { T *a; int n, cap; }
#define VPUSH(v, x)                                                     \
  do {                                                                  \
    if ((v).n == (v).cap) {                                             \
      (v).cap = (v).cap ? 2 * (v).cap : 8;                              \
      (v).a = xrealloc((v).a, (size_t)(v).cap * sizeof *(v).a);         \
    }                                                                   \
    (v).a[(v).n++] = (x);                                               \
  } while (0)

typedef struct { char *s; size_t n, cap; } SB;
void sb_puts(SB *b, const char *s);
void sb_printf(SB *b, const char *fmt, ...);

/* Interned-key -> int map. map_put logs the old value so a failed REPL
   input can be rolled back with undo_to. */
typedef struct { const char **keys; int *vals; int cap, n; } Map;
int map_get(Map *m, const char *key); /* -1 if absent */
void map_put(Map *m, const char *key, int val);
int undo_mark(void);
void undo_to(int mark);

extern const char *cur_file;
extern jmp_buf *cerr_jmp;
_Noreturn void cerror(int line, int col, const char *fmt, ...);
void cwarning(int line, int col, const char *fmt, ...);

/* ---------------------------------------------------------------- tokens */

typedef enum {
  T_EOF, T_VARID, T_CONID, T_VARSYM, T_CONSYM, T_INT, T_FLOAT, T_CHAR, T_STRING,
  T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_COMMA, T_SEMI, T_BACKTICK,
  T_LBRACE, T_RBRACE, T_VLBRACE, T_VRBRACE,
  T_EQ, T_BAR, T_ARROW, T_LAMBDA, T_DCOLON, T_DOTDOT, T_DARROW, T_UNDERSCORE,
  T_CASE, T_OF, T_LET, T_IN, T_WHERE, T_IF, T_THEN, T_ELSE, T_DATA, T_DERIVING,
  T_INFIXL, T_INFIXR, T_INFIX,
  T_FIELD, /* .name directly after a token: field access or qualified name */
} TokKind;

typedef struct {
  TokKind k;
  const char *s; /* interned name/operator, or string literal bytes */
  int len;       /* string literal length */
  long long ival;
  double fval;
  int line, col;
  bool bol; /* first token on its line */
} Token;

Token *lex(const char *src, size_t len, int *ntok); /* includes layout */
const char *tok_name(TokKind k);

/* ------------------------------------------------------------------- AST */

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Pat Pat;
typedef struct Bind Bind;
typedef struct Alt Alt;
typedef struct Lambda Lambda;

enum { TE_VAR, TE_CON, TE_FUN, TE_LIST, TE_TUPLE };
typedef struct TypeExpr {
  int k, line, col;
  const char *name;
  int n;
  struct TypeExpr **args;
} TypeExpr;

typedef struct {
  int nctx;
  const char **ctx; /* type variables constrained by Num */
  TypeExpr *t;
  int line, col;
} Sig;

enum { P_VAR, P_WILD, P_INT, P_CHAR, P_CON };
struct Pat {
  int k, line, col;
  const char *name; /* variable or constructor */
  long long ival;
  int n;
  Pat **args;
  int con;  /* constructor id, set by check */
  int slot; /* frame slot of a variable, set by resolve */
  bool rec; /* C { f = p, ... }: args match fields, until check reorders them */
  const char **fields;
};

typedef struct { Bind **b; int n; bool rec; } Group;

/* Guarded right-hand side. guards[i] == NULL means unconditional.
   If every guard fails, matching falls through to the next alternative. */
typedef struct {
  Group *wg; /* where-bindings, one Group per SCC */
  int nwg;
  int ng;
  Expr **guards, **bodies;
} Rhs;

struct Alt { int np; Pat **pats; Rhs rhs; };

enum { E_VAR, E_CON, E_INT, E_FLOAT, E_CHAR, E_STR, E_APP, E_LAM, E_LET, E_IF, E_CASE, E_ANN,
       E_FIELD,   /* body.name; check lowers it to a case */
       E_RECORD }; /* fn { fields = args }: construction if fn is E_CON, else update */
enum { VK_NONE, VK_LOCAL, VK_FREE, VK_GLOBAL };

struct Expr {
  int k, line, col;
  const char *name; /* VAR/CON name, STR bytes, CASE source file */
  int slen;
  long long ival;
  double fval;
  Type *ty;       /* INT: literal type, decides Int vs Float; VAR: type of show/print */
  int vk, idx;    /* VAR: resolved location; CON: constructor id */
  bool lazy;      /* VAR: bound to a thunk, forced on load */
  const char *tdesc; /* VAR: show/print with a known argument type */
  Lambda *known;  /* VAR bound to a known lambda: direct calls */
  Expr *fn;       /* APP, RECORD */
  int n;
  Expr **args;
  const char **fields; /* RECORD */
  Lambda *lam;    /* LAM */
  Group g;        /* LET */
  Expr *body;     /* LET, ANN, FIELD */
  Expr *cond, *then_, *else_;
  int ns, na;     /* CASE */
  Expr **scruts;
  Alt **alts;
  Sig *sig;       /* ANN */
};

typedef struct { int vk, idx; } FreeSrc; /* where a closure gets a free var */

struct Lambda {
  int n;
  const char **params;
  Expr *body;
  int line, col;
  const char *name;
  int id;
  int nslots; /* params occupy slots 0..n-1 */
  int nfree;
  FreeSrc *free;
};

struct Bind {
  const char *name;
  Expr *e;
  Sig *sig;
  int line, col;
  int slot;     /* local frame slot */
  int gidx;     /* global index, for top-level binds */
  Lambda *init; /* top-level value: zero-arity body evaluated once */
  Lambda *thunk; /* local value: zero-arity body evaluated on first use */
};

typedef struct {
  const char *name;
  int nfields;
  TypeExpr **fields;
  const char **labels; /* field names, or NULL if positional */
  int line, col;
} ConDecl;
typedef struct {
  const char *name;
  int np;
  const char **params;
  int nc;
  ConDecl *cons;
  int line, col;
} DataDecl;

typedef struct {
  Bind **binds;
  int nb;
  DataDecl **datas;
  int nd;
} Module;

Module *parse_module(const char *src, size_t len);
Expr *parse_expr_text(const char *src, size_t len);
Sig *parse_sig_text(const char *src);
bool is_decl_text(const char *src, size_t len);

/* ----------------------------------------------------------------- check */

struct Type {
  int k; /* TY_VAR, TY_CON */
  Type *link;
  int level, id;
  bool num, skolem;
  const char *name;
  int n;
  Type **args;
};
enum { TY_VAR, TY_CON };

typedef struct {
  const char *name;
  Type *type; /* scheme: generic vars are quantified */
  int prim;   /* index into fk_prims, or -1 */
  Lambda *lam;
} Global;

typedef struct { const char *name; int arity; Type *scheme; int tydef; const char **labels; } ConDef;
typedef struct { const char *name; int arity, ncons; } TyDef;

typedef struct { Group *groups; int ng; } Chunk;

extern VEC(Global) globals;
extern VEC(ConDef) condefs;
extern VEC(TyDef) tydefs;
extern VEC(Lambda *) all_lambdas;

void compiler_init(void);
bool compile_chunk(Module *m, Chunk *out);
const char *infer_type_text(Expr *e);
int global_lookup(const char *name);
void global_alias(const char *name, int gidx);
Type *repr(Type *t);
const char *type_str(Type *t);
const char *show_desc(Type *t);

/* --------------------------------------------------------------- backends */

void resolve_top(Bind *b);
const char *prim_cname(int i);

void interp_install(Chunk *c);
V interp_global(int gidx);

char *cgen_program(Chunk *chunks, int nchunks);

extern const char fk_rt_src[];
extern const char fk_prelude_src[];

#endif
