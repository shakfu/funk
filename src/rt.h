/* rt.h -- funk runtime: values, GC, application, primitives.
   Linked into the interpreter and pasted verbatim into compiled programs,
   so both backends share one implementation of the semantics. */
#ifndef FK_RT_H
#define FK_RT_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

/* A value is a tagged word:
     ...1   63-bit Int (shifted left 1)
     ..10   Char (shifted left 2)
     ..00   pointer to a heap or static object */
typedef uintptr_t V;
typedef V (*FkCode)(V self, V *args);

typedef struct { uint8_t kind, pad; uint16_t n; uint32_t aux; } FkHdr;
enum { K_FREE, K_FLOAT, K_CON, K_CLOSURE, K_PAP, K_THUNK };
/* K_CON:     n = field count, aux = constructor id, fields follow
   K_FLOAT:   double follows
   K_CLOSURE: n = free count, aux = arity; then code, info, free vars
   K_PAP:     aux = held arg count; then closure, args
   K_THUNK:   closure layout, arity 0; once forced, code = 0 and info = value */
typedef struct { FkHdr h; double d; } FkFloat;

#define FK_ISINT(v)    ((v) & 1)
#define FK_MKINT(i)    ((((V)(intptr_t)(i)) << 1) | 1)
#define FK_INT(v)      (((intptr_t)(v)) >> 1)
#define FK_ISCHAR(v)   (((v) & 3) == 2)
#define FK_MKCHAR(c)   ((((V)(unsigned char)(c)) << 2) | 2)
#define FK_CHAR(v)     ((int)((v) >> 2))
#define FK_ISPTR(v)    (((v) & 3) == 0)
#define FK_HDR(v)      ((FkHdr *)(v))
#define FK_W(v)        ((V *)(v))
#define FK_FIELD(v, i) (FK_W(v)[1 + (i)])
#define FK_CONID(v)    (FK_HDR(v)->aux)
#define FK_FLOATV(v)   (((FkFloat *)(v))->d)
#define FK_CODE(c)     ((FkCode)FK_W(c)[1])
#define FK_INFO(c)     ((void *)FK_W(c)[2])
#define FK_FREE(c, i)  (FK_W(c)[3 + (i)])
#define FK_ARITY(c)    (FK_HDR(c)->aux)

/* Builtin constructor ids, registered by fk_init in this order. */
enum { FK_C_FALSE, FK_C_TRUE, FK_C_UNIT, FK_C_NIL, FK_C_CONS, FK_C_TUP2 };
#define FK_MAXTUPLE 7
#define FK_NBUILTIN_CONS (FK_C_TUP2 + FK_MAXTUPLE - 1)

typedef struct { const char *name; int arity; V nullary, fn; } FkCon;
extern FkCon *fk_cons;
extern uint32_t fk_ncons;
extern V fk_true, fk_false, fk_unit, fk_nil;
#define FK_BOOL(b) ((b) ? fk_true : fk_false)

void fk_init(void);
int fk_run(void (*fn)(void *), void *arg); /* run fn on a large stack */
void fk_add_roots(V **base, size_t *count);

V fk_alloc(size_t words, int kind, int n, uint32_t aux);
V fk_float(double d);
V fk_mkcon(uint32_t id, int n, V *fields);
V fk_con_value(uint32_t id);
V fk_closure(FkCode code, int arity, int nfree, void *info);
V fk_apply(V f, int n, V *args);
V fk_thunk(FkCode code, int nfree, void *info);
V fk_force(V t);
uint32_t fk_register_con(const char *name, int arity);
void fk_truncate_cons(uint32_t n);

V fk_string(const char *s, size_t n);
char *fk_cstring(V list, size_t *len);
char *fk_show_cstr(V v);
char *fk_show_typed_cstr(const char *desc, V v); /* desc NULL: structural */
V fk_show_typed(const char *desc, V v);
V fk_print_typed(const char *desc, V v);
V fk_typed_show_fn(int print, const char *desc);
int fk_compare(V a, V b);
int fk_num_eq_int(V v, long long i);

extern jmp_buf *fk_error_jmp;
_Noreturn void fk_error(const char *fmt, ...);
_Noreturn void fk_match_fail(const char *where);
_Noreturn void fk_stack_overflow(void);
extern char *fk_stack_limit;
#define FK_STACK_CHECK() \
  do { char c_; if ((char *)&c_ < fk_stack_limit) fk_stack_overflow(); } while (0)

typedef struct { const char *name; int arity; FkCode code; } FkPrim;
extern const FkPrim fk_prims[];
extern const int fk_nprims;
V fk_prim_value(int i);

#define P(c, name, ar, ty) V fk_p_##c(V self, V *a);
#include "prims.def"
#undef P

#endif
