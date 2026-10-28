/* rt.h -- funk runtime: values, GC, application, primitives.
   Linked into the interpreter and pasted verbatim into compiled programs,
   so both backends share one implementation of the semantics. */
#ifndef FK_RT_H
#define FK_RT_H

#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
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
V fk_s_showint(int64_t i); /* show at Int */
static inline V FK_MKINT(int64_t i) {
  V w = ((V)(uint64_t)i << 1) | 1;
  return __builtin_expect(((int64_t)w >> 1) == i, 1) ? w : fk_int64_box(i); /* the hint: about 2% (typed-backend.md) */
}
static inline int64_t FK_INT(V v) { return __builtin_expect(FK_ISSMALL(v), 1) ? (int64_t)v >> 1 : ((FkInt64 *)v)->i; }
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
V fk_ptr(void *p); /* not const: GCC would take the memory for read, and warn that a fresh malloc is uninitialized */

/* Incremental marking (realtime.md, R2): a write that overwrites a heap
   pointer shades the old value while a cycle is marking. */
extern int fk_marking;
void fk_shade(V old);
#define FK_SHADE(old) do { if (fk_marking) fk_shade(old); } while (0)
const char *fk_cstr(V s);                     /* a String's bytes for C; an error if a NUL is inside */
V fk_cstr_result(const void *p, const char *fn); /* a C string copied into a String; an error if NULL */

/* Callbacks (ffi.md, phase 3) */
V fk_stable_deref(void *p);
bool fk_on_funk_thread(void);
_Noreturn void fk_callback_thread(const char *name);
extern int fk_cb_failed; /* a direct callback failed; its message is printed */
extern int fk_callback_depth; /* direct callbacks running, for the collector's stack snapshot */
extern char *fk_snap_lo;
void fk_snapshot_release(char *sp);
/* Before a foreign call: no read-only snapshot page below this frame (realtime.md). */
static inline __attribute__((unused)) void fk_ffi_enter(void) { /* also on an export's thread, while funk's collects */
  char *lo = __atomic_load_n(&fk_snap_lo, __ATOMIC_RELAXED);
  if (lo && (char *)__builtin_frame_address(0) > lo) fk_snapshot_release((char *)__builtin_frame_address(0));
}
_Noreturn void fk_cb_rethrow(void);
bool fk_on_funk_thread(void);
/* the failure is funk's thread's: an export on another thread leaves it */
static inline __attribute__((unused)) void fk_cb_check(void) { if (fk_cb_failed && fk_on_funk_thread()) fk_cb_rethrow(); }
/* A slot of the queued-callback ring (realtime.md, R3). The trampoline
   writes its arguments into data; bytes that do not fit are malloc'd. */
#define FK_EVENT_DATA 104
typedef struct FkEvent {
  _Atomic size_t seq;
  void (*run)(struct FkEvent *); /* NULL: dropped, its bytes could not be copied */
  void *ctx;
  _Alignas(16) unsigned char data[FK_EVENT_DATA];
} FkEvent;
FkEvent *fk_event_claim(void);     /* any thread: a slot, or NULL if the ring is full (counted) */
void fk_event_publish(FkEvent *e); /* any thread */
void fk_event_release(FkEvent *e); /* funk's thread, once the arguments are read */
void *fk_event_copy(FkEvent *e, size_t *used, const void *p, size_t n); /* into data if it fits; NULL if p is or malloc fails */
void fk_event_uncopy(FkEvent *e, void *p);

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
/* Nullary constructors' objects, from a constant table in generated code,
   so a case on one folds; set before fk_run. The interpreter makes its own. */
typedef struct { FkHdr h; V pad; } FkNullary;
void fk_nullary_table(const FkNullary *t, uint32_t n);
extern uint32_t fk_ncons;
extern V fk_true, fk_false, fk_unit, fk_nil;
#define FK_BOOL(b) ((b) ? fk_true : fk_false)
/* peek and poke on C addresses, for typed code: the width in bytes,
   negative for an unsigned integer */
static inline int64_t fk_s_peeki(int64_t w, void *p) {
  switch (w) {
  case 1: return *(int8_t *)p;
  case -1: return *(uint8_t *)p;
  case 2: return *(int16_t *)p;
  case -2: return *(uint16_t *)p;
  case 4: return *(int32_t *)p;
  case -4: return *(uint32_t *)p;
  default: return *(int64_t *)p;
  }
}
static inline double fk_s_peekf(int64_t w, void *p) { return w == 4 ? (double)*(float *)p : *(double *)p; }
static inline V fk_s_pokei(int64_t w, void *p, int64_t x) {
  switch (w) {
  case 1: case -1: *(uint8_t *)p = (uint8_t)x; break;
  case 2: case -2: *(uint16_t *)p = (uint16_t)x; break;
  case 4: case -4: *(uint32_t *)p = (uint32_t)x; break;
  default: *(int64_t *)p = x;
  }
  return fk_unit;
}
static inline V fk_s_pokef(int64_t w, void *p, double x) {
  if (w == 4) *(float *)p = (float)x;
  else *(double *)p = x;
  return fk_unit;
}
/* Array a and Ring a (realtime.md): C memory, a header and then the
   elements, never moved or collected; freed with freeArray and freeRing.
   A Ring has one producer and one consumer: head and tail count pushes
   and pops, and each side writes only its own, with release order. */
typedef struct { int64_t length, size; } FkArray;
typedef struct { _Atomic int64_t head, tail; int64_t cap, size; } FkRing; /* cap: a power of two */
static inline int64_t fk_s_alen(void *a) { return ((FkArray *)a)->length; }
static inline int64_t fk_s_asize(void *a) { return ((FkArray *)a)->size; }
static inline void *fk_s_aptr(void *a) { return (char *)a + sizeof(FkArray); }
static inline int64_t fk_s_truncsat(double d) { /* toward zero, saturating at Int's range; NaN is 0 */
  if (!(d == d)) return 0;
  if (d >= 0x1p63) return INT64_MAX;
  if (d < -0x1p63) return INT64_MIN;
  return (int64_t)d;
}
static inline int64_t fk_s_wrap(int64_t i, int64_t n) { /* i modulo n, from 0; 0 if n is not positive */
  if (n <= 0) return 0;
  int64_t r = i % n;
  return r < 0 ? r + n : r;
}
static inline int64_t fk_s_rcount(void *r) {
  FkRing *g = r;
  return atomic_load_explicit(&g->head, memory_order_acquire) - atomic_load_explicit(&g->tail, memory_order_acquire);
}
static inline int64_t fk_s_rspace(void *r) { return ((FkRing *)r)->cap - fk_s_rcount(r); }
static inline void *fk_s_rslot(FkRing *g, int64_t n) { return (char *)(g + 1) + (n & (g->cap - 1)) * g->size; }
static inline void *fk_s_rback(void *r) {
  FkRing *g = r;
  return fk_s_rslot(g, atomic_load_explicit(&g->head, memory_order_relaxed));
}
static inline V fk_s_rcommit(void *r) { /* the producer: the back slot is written */
  FkRing *g = r;
  if (fk_s_rspace(r) > 0) atomic_store_explicit(&g->head, atomic_load_explicit(&g->head, memory_order_relaxed) + 1, memory_order_release);
  return fk_unit;
}
static inline void *fk_s_rfront(void *r) {
  FkRing *g = r;
  return fk_s_rslot(g, atomic_load_explicit(&g->tail, memory_order_relaxed));
}
static inline V fk_s_rdrop(void *r) { /* the consumer: the front slot is read */
  FkRing *g = r;
  if (fk_s_rcount(r) > 0) atomic_store_explicit(&g->tail, atomic_load_explicit(&g->tail, memory_order_relaxed) + 1, memory_order_release);
  return fk_unit;
}

/* Atomic peek and poke: an acquire load and a release store, so a value
   written by one thread is read whole by another, with what was written
   before it. */
static inline int64_t fk_s_apeeki(int64_t w, void *p) {
  switch (w) {
  case 1: return __atomic_load_n((int8_t *)p, __ATOMIC_ACQUIRE);
  case -1: return __atomic_load_n((uint8_t *)p, __ATOMIC_ACQUIRE);
  case 2: return __atomic_load_n((int16_t *)p, __ATOMIC_ACQUIRE);
  case -2: return __atomic_load_n((uint16_t *)p, __ATOMIC_ACQUIRE);
  case 4: return __atomic_load_n((int32_t *)p, __ATOMIC_ACQUIRE);
  case -4: return __atomic_load_n((uint32_t *)p, __ATOMIC_ACQUIRE);
  default: return __atomic_load_n((int64_t *)p, __ATOMIC_ACQUIRE);
  }
}
static inline V fk_s_apokei(int64_t w, void *p, int64_t x) {
  switch (w) {
  case 1: case -1: __atomic_store_n((uint8_t *)p, (uint8_t)x, __ATOMIC_RELEASE); break;
  case 2: case -2: __atomic_store_n((uint16_t *)p, (uint16_t)x, __ATOMIC_RELEASE); break;
  case 4: case -4: __atomic_store_n((uint32_t *)p, (uint32_t)x, __ATOMIC_RELEASE); break;
  default: __atomic_store_n((int64_t *)p, x, __ATOMIC_RELEASE);
  }
  return fk_unit;
}
static inline double fk_s_apeekf(int64_t w, void *p) {
  if (w == 4) {
    uint32_t b = __atomic_load_n((uint32_t *)p, __ATOMIC_ACQUIRE);
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
  }
  uint64_t b = __atomic_load_n((uint64_t *)p, __ATOMIC_ACQUIRE);
  double d;
  memcpy(&d, &b, sizeof d);
  return d;
}
/* Atomic read-modify-write, sequentially consistent: the old value. */
static inline int64_t fk_s_aaddi(int64_t w, void *p, int64_t x) {
  switch (w) {
  case 1: return __atomic_fetch_add((int8_t *)p, (int8_t)x, __ATOMIC_SEQ_CST);
  case -1: return __atomic_fetch_add((uint8_t *)p, (uint8_t)x, __ATOMIC_SEQ_CST);
  case 2: return __atomic_fetch_add((int16_t *)p, (int16_t)x, __ATOMIC_SEQ_CST);
  case -2: return __atomic_fetch_add((uint16_t *)p, (uint16_t)x, __ATOMIC_SEQ_CST);
  case 4: return __atomic_fetch_add((int32_t *)p, (int32_t)x, __ATOMIC_SEQ_CST);
  case -4: return __atomic_fetch_add((uint32_t *)p, (uint32_t)x, __ATOMIC_SEQ_CST);
  default: return (int64_t)__atomic_fetch_add((uint64_t *)p, (uint64_t)x, __ATOMIC_SEQ_CST); /* wraps */
  }
}
static inline bool fk_s_acasi(int64_t w, void *p, int64_t old, int64_t x) {
  switch (w) {
  case 1: case -1: { uint8_t o = (uint8_t)old; return __atomic_compare_exchange_n((uint8_t *)p, &o, (uint8_t)x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
  case 2: case -2: { uint16_t o = (uint16_t)old; return __atomic_compare_exchange_n((uint16_t *)p, &o, (uint16_t)x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
  case 4: case -4: { uint32_t o = (uint32_t)old; return __atomic_compare_exchange_n((uint32_t *)p, &o, (uint32_t)x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
  default: { uint64_t o = (uint64_t)old; return __atomic_compare_exchange_n((uint64_t *)p, &o, (uint64_t)x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
  }
}
/* Floats: a compare-and-swap on the bits; compare-and-swap compares bits too, so -0.0 is not 0.0 */
static inline double fk_s_aaddf(int64_t w, void *p, double x) {
  if (w == 4) {
    uint32_t o = __atomic_load_n((uint32_t *)p, __ATOMIC_RELAXED), n;
    float f;
    do { memcpy(&f, &o, sizeof f); float g = (float)(f + x); memcpy(&n, &g, sizeof n); }
    while (!__atomic_compare_exchange_n((uint32_t *)p, &o, n, true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    return f;
  }
  uint64_t o = __atomic_load_n((uint64_t *)p, __ATOMIC_RELAXED), n;
  double d;
  do { memcpy(&d, &o, sizeof d); double g = d + x; memcpy(&n, &g, sizeof n); }
  while (!__atomic_compare_exchange_n((uint64_t *)p, &o, n, true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
  return d;
}
static inline bool fk_s_acasf(int64_t w, void *p, double old, double x) {
  if (w == 4) {
    float fo = (float)old, fx = (float)x;
    uint32_t o, n;
    memcpy(&o, &fo, sizeof o), memcpy(&n, &fx, sizeof n);
    return __atomic_compare_exchange_n((uint32_t *)p, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  }
  uint64_t o, n;
  memcpy(&o, &old, sizeof o), memcpy(&n, &x, sizeof n);
  return __atomic_compare_exchange_n((uint64_t *)p, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline V fk_s_apokef(int64_t w, void *p, double x) {
  if (w == 4) {
    float f = (float)x;
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    __atomic_store_n((uint32_t *)p, b, __ATOMIC_RELEASE);
  } else {
    uint64_t b;
    memcpy(&b, &x, sizeof b);
    __atomic_store_n((uint64_t *)p, b, __ATOMIC_RELEASE);
  }
  return fk_unit;
}
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
/* An error funk cannot recover from: fk_fatal_hook (libfunk.h, funk_on_fatal) runs, then the process exits. */
extern void (*fk_fatal_hook)(void *ctx, const char *msg);
extern void *fk_fatal_ctx;
_Noreturn void fk_fatal(const char *msg);
void fk_set_memory(size_t heap, size_t threshold, size_t stack);
void fk_recover(void); /* after a runtime error has unwound: a full heap's memory collected */
/* Set from any thread (libfunk.h, funk_interrupt); the interpreter checks it at each call. */
extern atomic_int fk_interrupt;
/* Output: stream 1 the program's, 2 messages. To fk_out if set (libfunk.h,
   funk_output), else stdout and stderr. A message is one write, newline included. */
extern void (*fk_out)(void *ctx, int stream, const char *s, size_t n);
extern void *fk_out_ctx;
void fk_write(int stream, const char *s, size_t n);
void fk_message(const char *prefix, const char *fmt, ...);
void fk_vmessage(const char *prefix, const char *fmt, va_list ap);
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
