/* elab.h -- the state and helpers shared by the passes after inference:
   dictionary passing (elab.c), specialization on function arguments
   (ho.c), lambda lifting (lift.c) and reuse of list cells (reuse.c). */
#ifndef ELAB_H
#define ELAB_H
#include "funk.h"

typedef struct { Type *from, *to; } TS;

typedef struct Nm { const char *name; struct Nm *next; } Nm;

/* Local variables of a call site that an argument uses, in order. */
typedef struct { VEC(const char *) names; VEC(Type *) types; } Fv;

/* Versions: a function and its parameters' modes, 2 bits each. */
typedef struct { Bind *f; uint32_t key; int res; Bind *copy; } Ver;

/* The passes' state, one per session (embedding.md, phase 2) */
typedef struct {
  int dcounter;
  TS *tsub; /* type variables of the function being specialized */
  bool default_value; /* elab_default_value: this chunk's value may be cloned */
  int ntsub;
  int nho; /* this chunk's copies, bounded so argument growth terminates */
  bool ho_capped; /* the bound was reached in this chunk, and reported */
  Fv *key_fv;
  Nm *keep_locals; /* the copy's leading parameters, during replace */
  VEC(Bind *) ho_work;
  int nhparam;
  int nbeta;
  int nlift;
  VEC(Bind *) lifted;
  VEC(Ver) vers;
  bool vers_new; /* the fixpoint met a version it had not seen */
  bool vers_done; /* the fixpoint is over: an unseen version is unowned */
  VEC(Bind *) own_work;
  VEC(void *) own_blocks; /* reuse_cells' Own nodes, freed when it ends */
  int own_left;           /* nodes free in the last block */
} ElabState;
#define ST ((ElabState *)fk_here()->cc[CC_ELAB])

/* elab.c */
Expr *el_mk(int k, int line, int col);
Expr *el_var(const char *name, int line, int col); /* of type dict_type */
Type *el_apply(Type *t);                           /* t with tsub's variables replaced */
int el_ground_depth(Type *t);
void el_subst_types(Expr *e);
Nm *el_nm(Nm *s, const char *name);
bool el_in(Nm *s, const char *name);
Nm *el_pat_nm(Pat *p, Nm *s);
int el_fv_index(Fv *fv, const char *name);
/* reuse.c */
void reuse_free(void); /* reuse_cells' Own nodes */

#endif
