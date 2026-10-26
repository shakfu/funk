/* funk.h -- compiler data structures shared by all passes.
   Pipeline: lex+layout -> parse (desugar) -> check (SCC, inference)
             -> resolve (closure conversion) -> interp | cgen */
#ifndef FUNK_H
#define FUNK_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
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
typedef VEC(char *) Argv;
typedef VEC(const char *) Strs;
Argv cc_argv(void);      /* the words of $CC, or `cc` */
int spawn_wait(Argv *a); /* runs a, which it NULL-terminates; the exit status, or -1 */
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
extern bool cerr_quiet; /* suppress error messages (REPL probes) */
_Noreturn void cerror(int line, int col, const char *fmt, ...);
void cwarning(int line, int col, const char *fmt, ...);

/* ---------------------------------------------------------------- tokens */

typedef enum {
  T_EOF, T_VARID, T_CONID, T_VARSYM, T_CONSYM, T_INT, T_FLOAT, T_CHAR, T_STRING,
  T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_COMMA, T_SEMI, T_BACKTICK,
  T_LBRACE, T_RBRACE, T_VLBRACE, T_VRBRACE,
  T_EQ, T_BAR, T_ARROW, T_LAMBDA, T_DCOLON, T_DOTDOT, T_DARROW, T_UNDERSCORE,
  T_CASE, T_OF, T_LET, T_IN, T_WHERE, T_IF, T_THEN, T_ELSE, T_DATA, T_DERIVING,
  T_INFIXL, T_INFIXR, T_INFIX, T_CLASS, T_INSTANCE, T_DO, T_LARROW,
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
  const char **ctx_cls, **ctx; /* constraint i is `ctx_cls[i] ctx[i]` */
  TypeExpr *t;
  int line, col;
} Sig;

enum { P_VAR, P_WILD, P_INT, P_CHAR, P_CON, P_STR }; /* P_STR: name holds ival bytes */
struct Pat {
  int k, line, col;
  const char *name; /* variable or constructor */
  long long ival;
  int n;
  Pat **args;
  int con;  /* constructor id, set by check */
  int slot; /* frame slot of a variable, set by resolve */
  Type *ty; /* type after inference */
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
  V lit;            /* STR: the String, made once on first evaluation (interpreter) */
  int slen;
  long long ival;
  double fval;
  Type *ty;       /* type after inference; INT: decides Int, Float or fromInt */
  int vk, idx;    /* VAR: resolved location; CON: constructor id */
  bool lazy;      /* VAR: bound to a thunk, forced on load */
  bool global_ref; /* VAR: refers to the global of this name, even if a local shadows it */
  bool own;       /* VAR scrutinee: a matched cons cell is dead and may be reused (reuse.md) */
  struct Pred *preds; /* VAR: class constraints of this instantiation */
  int npreds;
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
  Type **ptypes; /* parameter types after inference; NULL entries for dictionaries */
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
  Type *scheme;  /* given type, like a signature (generated bindings) */
  Type *type;    /* inferred or given type, for elaboration */
  Type **skolem; /* rigid copies of scheme variables: pairs generic, copy */
  int nskolem;
  struct Pred *preds; /* constraints generalized at this binding: its dictionary parameters */
  int npreds;
  Expr *orig;    /* overloaded function before elaboration, copied for specializations */
  bool fresh;    /* returns only cells it allocated (reuse.md) */
  uint32_t consumes, gain; /* bit i: list parameter i is consuming; and reusing it can pay */
  int reuse;     /* a reuse copy: 1 + the parameter whose cells it reuses */
  struct Bind *from; /* type-class clone: the definition it was cloned from */
  bool pure, total;  /* no effects; and cannot fail, given such arguments (fusion.md) */
  bool prelude;      /* defined in the prelude */
  bool acts;         /* a `let` statement in `do` whose evaluation can act: evaluated on its line, not lazily */
  bool stmt;         /* bound by a `let` statement in a `do` block */
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
  int nderiv;
  const char **derivs;
  int derive_line, derive_col;
} DataDecl;

typedef struct {
  const char *name, *var;
  int nsup;
  const char **sups;
  int nsig;
  const char **names; /* method i is names[i] :: sigs[i] */
  Sig **sigs;
  int ndef;
  Bind **defs; /* default methods */
  int line, col;
} ClassDecl;

typedef struct {
  const char *cls;
  TypeExpr *head;
  Sig *ctx; /* context only; t unused */
  int nb;
  Bind **binds;
  int line, col;
} InstDecl;

/* foreign import ccall "header.h cname" name :: type (ffi.md) */
typedef struct {
  const char *name, *header, *cname;
  Sig *sig;
  int line, col;
  int gidx, arity;
  bool pure;   /* written with `->` only */
  FkCode code; /* interpreter: the loaded glue's entry */
} ForeignDecl;

typedef struct {
  Bind **binds;
  int nb;
  DataDecl **datas;
  int nd;
  ClassDecl **classes;
  int ncl;
  InstDecl **insts;
  int ni;
  ForeignDecl **foreigns;
  int nforeign;
  const char **libs; /* foreign library "name" */
  int nlibs;
} Module;

Module *parse_module(const char *src, size_t len);
Expr *parse_expr_text(const char *src, size_t len);
Sig *parse_sig_text(const char *src);
bool is_decl_text(const char *src, size_t len);
char *builtin_instances_src(void);

/* ----------------------------------------------------------------- check */

struct Type {
  int k; /* TY_VAR, TY_CON */
  Type *link;
  int level, id;
  uint64_t cls; /* classes a variable must be an instance of, closed under superclasses */
  bool skolem;
  int line, col; /* where a constrained variable arose, for ambiguity errors */
  const char *vname; /* signature variable: its name, for messages */
  const char *name;
  int n;
  Type **args;
  struct Type *eff; /* an arrow's effect under FK_EFFECTS: Pure, Eff or a variable (typed-effects.md, E) */
};
enum { TY_VAR, TY_CON };

typedef struct {
  const char *name;
  Type *type; /* scheme: generic vars are quantified */
  int prim;   /* index into fk_prims, or -1 */
  Lambda *lam;
  const char *spec; /* prelude overload with primitives primSPECInt etc. */
  Bind *bind;       /* top-level binding, for specialization */
  int mcls, midx;   /* class method: class and method index, else -1 */
  ForeignDecl *ffi; /* a foreign import */
} Global;

typedef struct { const char *name; int arity; Type *scheme; int tydef; const char **labels; } ConDef;
typedef struct { const char *name; int arity, ncons; } TyDef;

typedef struct Pred { Type *t; int cls; } Pred;

typedef struct {
  const char *name;
  uint64_t closure; /* this class and its superclasses */
  int nsup;
  int *sups; /* direct superclasses, stored after the methods */
  int nmeth;
  const char **meths;
  int *arity;
  bool *hasdef;  /* has a default method $def.C.m */
  Type **mtypes; /* method types over `var`, a generic variable in this class */
  Type *var;
  int con; /* dictionary constructor */
} ClassDef;

typedef struct {
  int cls;
  const char *tycon;
  int arity;
  Type **vars; /* generic head variables; vars[i]->cls is the context */
  int dict;    /* global holding the dictionary or the function building it */
  int line, col;
} InstDef;

/* Dictionaries, selectors: bindings built after inference. */
typedef struct { Group *groups; int ng; Bind **raw; int nraw; } Chunk;
extern Strs ffi_libs;    /* libraries named by `foreign library`, in all chunks so far */
extern Strs ffi_cc_args; /* -I, -l and -L given to funk run or repl */

extern VEC(Global) globals;
extern VEC(ConDef) condefs;
extern VEC(TyDef) tydefs;
extern VEC(Lambda *) all_lambdas;
extern VEC(ClassDef) classes;
extern VEC(InstDef) insts;

void compiler_init(void);
bool compile_chunk(Module *m, Chunk *out);
const char *infer_type_text(Expr *e);
int global_lookup(const char *name);
void global_alias(const char *name, int gidx);
Type *repr(Type *t);
const char *type_str(Type *t);
int preds_of(Type *scheme, Pred **out);
InstDef *find_inst(int cls, const char *tycon);
Type *tcon_new(const char *name, int n, Type **args);

/* ---------------------------------------------------------- elaboration */

extern int num_class;
extern Type *dict_type; /* type of dictionary arguments: represented as V */
void check_types(Bind **bs, int n); /* report expressions without a type (FK_CHECK_TYPES) */
int global_add(const char *name, Type *t, Lambda *lam);
void elab_bind(Bind *b);
Bind *dict_bind(InstDef *in);
Expr *copy_expr(Expr *e);
void ho_specialize(Bind **bs, int n); /* copies made go to `specialized` */
void lift_locals(Bind **bs, int n);   /* lifted functions go to `specialized` */
void reuse_cells(Bind **bs, int n, Bind **cs, int nc); /* copies go to `specialized` */
int fuse_lists(Bind **all, int n); /* moves the changed bindings to the front; returns their count */
bool case_covers(Expr *e, bool otherwise_true); /* exhaustive, guards included; before resolve */
extern VEC(Bind *) specialized; /* clones made by elab_bind, to install */

/* --------------------------------------------------------------- backends */

void resolve_top(Bind *b);
const char *prim_cname(int i);

void interp_install(Chunk *c);
V interp_global(int gidx);

char *cgen_program(Chunk *chunks, int nchunks);

extern const char fk_rt_src[];
extern const char fk_rth_src[]; /* rt.h, for the interpreter's foreign glue */

/* ffi.c: foreign imports */
char *ffi_glue(ForeignDecl **fs, int n, bool exported); /* the C wrappers fk_ffi_GIDX */
const char *ffi_param_type(Type *t);                    /* NULL if t cannot be passed to C */
int ffi_kind(Type *t);                                  /* 'i' held as Int, 'f' as Float, 'c', 'b', 'v' (); 0 */
typedef struct { const char *name, *ctype; char kind; } FfiType;
extern const FfiType ffi_types[];
extern const int ffi_ntypes;
void ffi_load(ForeignDecl **fs, int n);                 /* interpreter: compile, cache, load; sets each code */
extern const char fk_prelude_src[];

#endif
