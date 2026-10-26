/* rt.h -- funk runtime: values, GC, application, primitives.
   Linked into the interpreter and pasted verbatim into compiled programs,
   so both backends share one implementation of the semantics. */
#ifndef FK_RT_H
#define FK_RT_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A value is a tagged word:
     ...1   63-bit Int (shifted left 1)
     ..10   Char (shifted left 2)
     ..00   pointer to a heap or static object */
typedef uintptr_t V;
typedef V (*FkCode)(V self, V *args);

typedef struct { uint8_t kind, pad; uint16_t n; uint32_t aux; } FkHdr;
enum { K_FREE, K_FLOAT, K_CON, K_CLOSURE, K_PAP, K_THUNK, K_STR, K_INT64, K_PTR };
/* K_CON:     n = field count, aux = constructor id, fields follow
   K_FLOAT:   double follows
   K_CLOSURE: n = free count, aux = arity; then code, info, free vars
   K_PAP:     aux = held arg count; then closure, args
   K_THUNK:   closure layout, arity 0; once forced, code = 0 and info = value
   K_STR:     aux = byte length; the bytes follow, NUL-terminated
   K_INT64:   an Int outside the 63 bits a tagged word holds follows
   K_PTR:     a C address follows (Ptr a); not scanned */
typedef struct { FkHdr h; double d; } FkFloat;
typedef struct { FkHdr h; int64_t i; } FkInt64;
typedef struct { FkHdr h; void *p; } FkPtr;

/* Int is 64 bits in both backends. A value that fits in 63 bits is a
   tagged word; others are boxed, so the common case does not allocate. */
#define FK_ISSMALL(v)  ((v) & 1)
#define FK_ISINT(v)    (FK_ISSMALL(v) || (FK_ISPTR(v) && FK_HDR(v)->kind == K_INT64))
V fk_int64_box(int64_t i);
static inline V FK_MKINT(int64_t i) {
  V w = ((V)(uint64_t)i << 1) | 1;
  return ((int64_t)w >> 1) == i ? w : fk_int64_box(i);
}
static inline int64_t FK_INT(V v) { return FK_ISSMALL(v) ? (int64_t)v >> 1 : ((FkInt64 *)v)->i; }
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
/* A double's bits in a closure slot, and back (typed-backend.md, step 4). */
static inline V fk_fraw(double d) { V v; memcpy(&v, &d, sizeof v); return v; }
static inline double fk_rawf(V v) { double d; memcpy(&d, &v, sizeof d); return d; }
#define FK_ARITY(c)    (FK_HDR(c)->aux)
#define FK_STRLEN(s)   ((size_t)FK_HDR(s)->aux)
#define FK_STRBYTES(s) ((char *)&FK_W(s)[1])
#define FK_PTRV(v)     (((FkPtr *)(v))->p)
V fk_ptr(const void *p);
const char *fk_cstr(V s);                     /* a String's bytes for C; an error if a NUL is inside */
V fk_cstr_result(const void *p, const char *fn); /* a C string copied into a String; an error if NULL */

/* Callbacks (ffi.md, phase 3) */
V fk_stable_deref(void *p);
bool fk_on_funk_thread(void);
_Noreturn void fk_callback_thread(const char *name);
extern int fk_cb_failed; /* a direct callback failed; its message is printed */
_Noreturn void fk_cb_rethrow(void);
static inline void fk_cb_check(void) { if (fk_cb_failed) fk_cb_rethrow(); }
typedef struct FkEvent { struct FkEvent *next; void (*run)(struct FkEvent *); void *ctx; } FkEvent;
void fk_event_push(FkEvent *e); /* any thread */
char *fk_memdup(const void *p, size_t n); /* malloc'd, NUL-terminated; aborts if out of memory */
_Noreturn void fk_callback_oom(const char *name);

/* Builtin constructor ids, registered by fk_init in this order. */
enum { FK_C_FALSE, FK_C_TRUE, FK_C_UNIT, FK_C_NIL, FK_C_CONS, FK_C_TUP2 };
#define FK_MAXTUPLE 7
#define FK_NBUILTIN_CONS (FK_C_TUP2 + FK_MAXTUPLE - 1)

typedef struct {
  const char *name;
  int arity;
  V nullary, fn;
  const char *const *labels;
  uint32_t raw; /* bit i: field i holds a raw double, in compiled code (typed-backend.md, step 5) */
} FkCon;
extern FkCon *fk_cons;
extern uint32_t fk_ncons;
extern V fk_true, fk_false, fk_unit, fk_nil;
#define FK_BOOL(b) ((b) ? fk_true : fk_false)
#define FK_ISTRUE(v) ((v) == fk_true)

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
void fk_con_raw(uint32_t id, uint32_t mask);
void fk_con_labels(uint32_t id, const char *const *labels); /* show as a record */
void fk_truncate_cons(uint32_t n);

V fk_str(const char *s, size_t n);        /* a String on the heap */
V fk_str_static(const char *s, size_t n); /* a String literal: allocated once, never collected */
bool fk_str_eq_lit(V s, const char *lit, size_t n);
char *fk_cstring(V s, size_t *len);       /* malloc'd copy of a String's bytes */
char *fk_show_cstr(V v);
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
