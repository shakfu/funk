/* rt.c -- funk runtime. See rt.h for the value representation.

   GC: non-moving conservative mark-sweep. Small objects (<= MAX_SMALL
   words) live in 1 MiB chunks with one object size per chunk. Chunks are
   carved in order from one reserved address range, so any word maps to its
   chunk, and then to an object start, by arithmetic alone. Roots are the funk
   thread's whole stack (registers spilled by setjmp) plus registered global
   arrays. Conservative scanning lets compiled C code keep values in
   ordinary locals with no shadow stack. */
#define _DEFAULT_SOURCE
#include "rt.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------------------------------------------------------------- errors */

jmp_buf *fk_error_jmp;
char *fk_stack_limit;
static char *fk_stack_base;

_Noreturn void fk_error(const char *fmt, ...) {
  va_list ap;
  fflush(stdout);
  fputs("runtime error: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  if (fk_error_jmp) longjmp(*fk_error_jmp, 1);
  exit(1);
}

_Noreturn void fk_match_fail(const char *where) {
  fk_error("non-exhaustive patterns at %s", where);
}

_Noreturn void fk_stack_overflow(void) { fk_error("stack overflow"); }

static void *rt_realloc(void *p, size_t n) {
  p = realloc(p, n);
  if (!p) { fputs("funk: out of memory\n", stderr); exit(1); }
  return p;
}

/* -------------------------------------------------------------------- GC */

#define CHUNK_SIZE ((size_t)1 << 20)
#define MAX_SMALL 32

typedef struct { size_t words, nobj; char *objs; uint8_t *marks; uint64_t recip; } Chunk;
typedef struct { V *p; size_t words; uint8_t mark; } Large;
typedef struct { V **base; size_t *count; } Root;

static size_t heap_reserve = (size_t)64 << 30; /* address space only; MAP_NORESERVE */
static char *heap_base;  /* chunk i is at heap_base + i * CHUNK_SIZE */
static size_t nchunks;
static uintptr_t heap_lo = UINTPTR_MAX, heap_hi; /* chunks in use */
static V *freelist[MAX_SMALL + 1];
static Large *larges;
static size_t nlarge, caplarge;
static uintptr_t large_lo = UINTPTR_MAX, large_hi;
static Root *roots;
static size_t nroots;
/* Collect after max(128 MiB, 2x live) of allocation. Each collection scans
   the whole stack, which deep recursion makes large, so fewer collections
   pay off: 26-35% on sort and strings against 32 MiB and 1x, at about 2x
   the peak memory. */
#define GC_MIN ((size_t)128 << 20)
static size_t alloc_since_gc, gc_threshold = GC_MIN;
static V **mstack;
static size_t msp, mcap;

void fk_add_roots(V **base, size_t *count) {
  roots = rt_realloc(roots, (nroots + 1) * sizeof *roots);
  roots[nroots++] = (Root){base, count};
}

static void push_free(size_t words, V *o) {
  o[1] = (V)freelist[words];
  freelist[words] = o;
}

static void new_chunk(size_t words) {
  if (!heap_base) { /* reserve, halving on failure, then align to CHUNK_SIZE */
    char *r = MAP_FAILED;
    for (; r == MAP_FAILED && heap_reserve >= ((size_t)256 << 20); heap_reserve /= 2)
      r = mmap(NULL, heap_reserve + CHUNK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) { fputs("funk: cannot reserve heap\n", stderr); exit(1); }
    heap_reserve *= 2; /* undo the last halving */
    heap_base = (char *)(((uintptr_t)r + CHUNK_SIZE - 1) & ~(uintptr_t)(CHUNK_SIZE - 1));
    heap_lo = (uintptr_t)heap_base;
  }
  if ((nchunks + 1) * CHUNK_SIZE > heap_reserve) { fputs("funk: out of memory\n", stderr); exit(1); }
  char *base = heap_base + nchunks * CHUNK_SIZE; /* fresh pages are zero */
  Chunk *c = (Chunk *)base;
  size_t hdr = sizeof(Chunk), objbytes = words * sizeof(V);
  size_t nobj = (CHUNK_SIZE - hdr - 16) / (objbytes + 1);
  c->words = words;
  c->nobj = nobj;
  c->recip = ((uint64_t)1 << 32) / objbytes + 1; /* offset * recip >> 32 == offset / objbytes below 1 MiB */
  c->marks = (uint8_t *)(base + hdr);
  c->objs = base + ((hdr + nobj + 15) & ~(size_t)15);
  nchunks++;
  heap_hi = (uintptr_t)base + CHUNK_SIZE;
  for (size_t k = nobj; k-- > 0;) push_free(words, (V *)(c->objs + k * objbytes));
}

/* Map a candidate word to the object containing it, or NULL. */
static V *find_obj(uintptr_t w, uint8_t **mark, size_t *words) {
  if (w >= heap_lo && w < heap_hi) {
    Chunk *c = (Chunk *)(w & ~(uintptr_t)(CHUNK_SIZE - 1));
    if (w < (uintptr_t)c->objs) return NULL;
    size_t i = (size_t)(((uint64_t)(w - (uintptr_t)c->objs) * c->recip) >> 32);
    if (i >= c->nobj) return NULL;
    V *o = (V *)(c->objs + i * c->words * sizeof(V));
    if (((FkHdr *)o)->kind == K_FREE) return NULL;
    *mark = &c->marks[i];
    *words = c->words;
    return o;
  }
  if (w >= large_lo && w < large_hi) {
    size_t lo = 0, hi = nlarge;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      Large *l = &larges[mid];
      if (w < (uintptr_t)l->p) hi = mid;
      else if (w >= (uintptr_t)(l->p + l->words)) lo = mid + 1;
      else { *mark = &l->mark; *words = l->words; return l->p; }
    }
  }
  return NULL;
}

static void mark_word(uintptr_t w) {
  if (!FK_ISPTR(w)) return; /* Ints and Chars are tagged: never pointers */
  uint8_t *m;
  size_t words;
  V *o = find_obj(w, &m, &words);
  if (!o || *m) return;
  *m = 1;
  if (((FkHdr *)o)->kind == K_FLOAT) return; /* a double, no pointers */
  if (msp + 2 > mcap) {
    mcap = mcap ? 2 * mcap : 4096;
    mstack = rt_realloc(mstack, mcap * sizeof *mstack);
  }
  mstack[msp++] = o;
  mstack[msp++] = (V *)(uintptr_t)words;
}

static void scan_range(V *lo, V *hi) {
  for (V *p = lo; p < hi; p++) mark_word(*p);
}

static void drain(void) {
  while (msp) {
    size_t words = (size_t)(uintptr_t)mstack[--msp];
    V *o = mstack[--msp];
    scan_range(o + 1, o + words); /* every payload word, conservatively */
  }
}

static __attribute__((noinline)) void scan_stack(void) {
  V *sp = (V *)__builtin_frame_address(0);
  scan_range(sp, (V *)fk_stack_base);
}

static void sweep(void) {
  size_t live = 0;
  memset(freelist, 0, sizeof freelist);
  for (size_t ci = 0; ci < nchunks; ci++) {
    Chunk *c = (Chunk *)(heap_base + ci * CHUNK_SIZE);
    size_t bytes = c->words * sizeof(V);
    for (size_t i = c->nobj; i-- > 0;) {
      V *o = (V *)(c->objs + i * bytes);
      if (((FkHdr *)o)->kind != K_FREE) {
        if (c->marks[i]) { c->marks[i] = 0; live += bytes; continue; }
        o[0] = 0; /* K_FREE; constructors overwrite every payload word */
      }
      push_free(c->words, o);
    }
  }
  size_t j = 0;
  large_lo = UINTPTR_MAX;
  large_hi = 0;
  for (size_t i = 0; i < nlarge; i++) {
    if (!larges[i].mark) { free(larges[i].p); continue; }
    larges[i].mark = 0;
    live += larges[i].words * sizeof(V);
    larges[j++] = larges[i];
    if ((uintptr_t)larges[i].p < large_lo) large_lo = (uintptr_t)larges[i].p;
    uintptr_t end = (uintptr_t)(larges[i].p + larges[i].words);
    if (end > large_hi) large_hi = end;
  }
  nlarge = j;
  gc_threshold = 2 * live > GC_MIN ? 2 * live : GC_MIN;
}

static void fk_gc(void) {
  jmp_buf regs;
  setjmp(regs); /* spill callee-saved registers into this frame */
  __asm__ volatile("" : : "r"(&regs) : "memory");
  scan_stack();
  for (size_t i = 0; i < nroots; i++) scan_range(*roots[i].base, *roots[i].base + *roots[i].count);
  drain();
  sweep();
  alloc_since_gc = 0;
}

static V alloc_large(size_t words) {
  V *p = calloc(words, sizeof(V));
  if (!p) { fputs("funk: out of memory\n", stderr); exit(1); }
  if (nlarge == caplarge) {
    caplarge = caplarge ? 2 * caplarge : 16;
    larges = rt_realloc(larges, caplarge * sizeof *larges);
  }
  size_t i = nlarge++;
  while (i > 0 && larges[i - 1].p > p) { larges[i] = larges[i - 1]; i--; }
  larges[i] = (Large){p, words, 0};
  if ((uintptr_t)p < large_lo) large_lo = (uintptr_t)p;
  if ((uintptr_t)(p + words) > large_hi) large_hi = (uintptr_t)(p + words);
  return (V)p;
}

static __attribute__((noinline)) V alloc_slow(size_t words, int kind, int n, uint32_t aux) {
  V *o;
  if (alloc_since_gc > gc_threshold) fk_gc();
  if (words > MAX_SMALL) {
    o = (V *)alloc_large(words);
  } else {
    if (!freelist[words]) new_chunk(words);
    o = freelist[words];
    freelist[words] = (V *)o[1];
    o[1] = 0;
  }
  FkHdr *h = (FkHdr *)o;
  h->kind = (uint8_t)kind;
  h->n = (uint16_t)n;
  h->aux = aux;
  return (V)o;
}

/* The common case inline: a small object from a non-empty free list, with
   no collection due. */
static inline __attribute__((always_inline)) V alloc(size_t words, int kind, int n, uint32_t aux) {
  if (words < 2) words = 2;
  alloc_since_gc += words * sizeof(V);
  V *o;
  if (words > MAX_SMALL || alloc_since_gc > gc_threshold || !(o = freelist[words]))
    return alloc_slow(words, kind, n, aux);
  freelist[words] = (V *)o[1];
  o[1] = 0;
  FkHdr *h = (FkHdr *)o;
  h->kind = (uint8_t)kind;
  h->n = (uint16_t)n;
  h->aux = aux;
  return (V)o;
}

V fk_alloc(size_t words, int kind, int n, uint32_t aux) { return alloc(words, kind, n, aux); }

/* --------------------------------------------------------------- objects */

FkCon *fk_cons;
uint32_t fk_ncons;
static uint32_t capcons;
V fk_true, fk_false, fk_unit, fk_nil;

static V static_obj(size_t words, int kind, int n, uint32_t aux) {
  V *o = calloc(words < 2 ? 2 : words, sizeof(V));
  if (!o) { fputs("funk: out of memory\n", stderr); exit(1); }
  FkHdr *h = (FkHdr *)o;
  h->kind = (uint8_t)kind;
  h->n = (uint16_t)n;
  h->aux = aux;
  return (V)o;
}

uint32_t fk_register_con(const char *name, int arity) {
  if (fk_ncons == capcons) {
    capcons = capcons ? 2 * capcons : 64;
    fk_cons = rt_realloc(fk_cons, capcons * sizeof *fk_cons);
  }
  uint32_t id = fk_ncons++;
  fk_cons[id] = (FkCon){name, arity, arity == 0 ? static_obj(1, K_CON, 0, id) : 0, 0, NULL};
  return id;
}

void fk_con_labels(uint32_t id, const char *const *labels) { fk_cons[id].labels = labels; }

void fk_truncate_cons(uint32_t n) { fk_ncons = n; }

static V con_code(V self, V *a) {
  return fk_mkcon((uint32_t)(uintptr_t)FK_INFO(self), (int)FK_ARITY(self), a);
}

V fk_con_value(uint32_t id) {
  FkCon *c = &fk_cons[id];
  if (c->arity == 0) return c->nullary;
  if (!c->fn) {
    c->fn = static_obj(3, K_CLOSURE, 0, (uint32_t)c->arity);
    FK_W(c->fn)[1] = (V)con_code;
    FK_W(c->fn)[2] = (V)(uintptr_t)id;
  }
  return c->fn;
}

V fk_mkcon(uint32_t id, int n, V *fields) {
  if (n == 0) return fk_cons[id].nullary;
  V o = alloc(1 + (size_t)n, K_CON, n, id);
  memcpy(&FK_W(o)[1], fields, (size_t)n * sizeof(V));
  return o;
}

V fk_float(double d) {
  V o = alloc(2, K_FLOAT, 0, 0);
  FK_FLOATV(o) = d;
  return o;
}

V fk_closure(FkCode code, int arity, int nfree, void *info) {
  V c = alloc(3 + (size_t)nfree, K_CLOSURE, nfree, (uint32_t)arity);
  FK_W(c)[1] = (V)code;
  FK_W(c)[2] = (V)info;
  return c;
}

V fk_thunk(FkCode code, int nfree, void *info) {
  V t = alloc(3 + (size_t)nfree, K_THUNK, nfree, 0);
  FK_W(t)[1] = (V)code;
  FK_W(t)[2] = (V)info;
  return t;
}

/* Captures are cleared after forcing so the GC can reclaim them. */
V fk_force(V t) {
  if (FK_W(t)[1]) {
    V v = FK_CODE(t)(t, NULL);
    FK_W(t)[1] = 0;
    FK_W(t)[2] = v;
    memset(&FK_FREE(t, 0), 0, FK_HDR(t)->n * sizeof(V));
  }
  return FK_W(t)[2];
}

V fk_apply(V f, int n, V *args) {
  FK_STACK_CHECK();
  for (;;) {
    if (!FK_ISPTR(f)) fk_error("applied a non-function");
    FkHdr *h = FK_HDR(f);
    if (h->kind == K_CLOSURE) {
      int ar = (int)h->aux;
      if (n == ar) return FK_CODE(f)(f, args);
      if (n < ar) {
        V p = alloc(2 + (size_t)n, K_PAP, 0, (uint32_t)n);
        FK_W(p)[1] = f;
        memcpy(&FK_W(p)[2], args, (size_t)n * sizeof(V));
        return p;
      }
      f = FK_CODE(f)(f, args);
      args += ar;
      n -= ar;
    } else if (h->kind == K_PAP) {
      int k = (int)h->aux;
      V buf[k + n];
      memcpy(buf, &FK_W(f)[2], (size_t)k * sizeof(V));
      memcpy(buf + k, args, (size_t)n * sizeof(V));
      return fk_apply(FK_W(f)[1], k + n, buf);
    } else {
      fk_error("applied a non-function");
    }
  }
}

/* --------------------------------------------------------------- strings */

V fk_string(const char *s, size_t n) {
  V acc = fk_nil;
  while (n-- > 0) acc = fk_mkcon(FK_C_CONS, 2, (V[]){FK_MKCHAR(s[n]), acc});
  return acc;
}

typedef struct { char *s; size_t n, cap; } Buf;

static void buf_putc(Buf *b, char c) {
  if (b->n + 1 >= b->cap) {
    b->cap = b->cap ? 2 * b->cap : 64;
    b->s = rt_realloc(b->s, b->cap);
  }
  b->s[b->n++] = c;
  b->s[b->n] = 0;
}

static void buf_puts(Buf *b, const char *s) {
  while (*s) buf_putc(b, *s++);
}

char *fk_cstring(V list, size_t *len) {
  Buf b = {0};
  buf_putc(&b, 0);
  b.n = 0;
  while (FK_CONID(list) == FK_C_CONS) {
    buf_putc(&b, (char)FK_CHAR(FK_FIELD(list, 0)));
    list = FK_FIELD(list, 1);
  }
  if (len) *len = b.n;
  return b.s;
}

/* ------------------------------------------------------------------ show */

static bool is_float(V v) { return FK_ISPTR(v) && FK_HDR(v)->kind == K_FLOAT; }

/* Haskell's format: shortest round-trip digits, fixed notation for
   0.1 <= |d| < 10^7, otherwise d.ddde<exp>. */
static void show_double(Buf *b, double d) {
  if (isnan(d)) { buf_puts(b, "NaN"); return; }
  if (isinf(d)) { buf_puts(b, d < 0 ? "-Infinity" : "Infinity"); return; }
  if (d == 0) { buf_puts(b, signbit(d) ? "-0.0" : "0.0"); return; }
  char tmp[64];
  for (int p = 1; p <= 17; p++) {
    snprintf(tmp, sizeof tmp, "%.*e", p - 1, d);
    if (strtod(tmp, NULL) == d) break;
  }
  char *s = tmp, digits[32];
  int nd = 0;
  if (*s == '-') { buf_putc(b, '-'); s++; }
  for (; *s && *s != 'e'; s++)
    if (isdigit((unsigned char)*s)) digits[nd++] = *s;
  int e = atoi(s + 1);
  while (nd > 1 && digits[nd - 1] == '0') nd--;
  digits[nd] = 0;
  double ad = fabs(d);
  if (ad >= 0.1 && ad < 1e7) {
    if (e < 0) {
      buf_puts(b, "0.");
      for (int i = 0; i < -e - 1; i++) buf_putc(b, '0');
      buf_puts(b, digits);
    } else {
      for (int i = 0; i <= e; i++) buf_putc(b, i < nd ? digits[i] : '0');
      buf_putc(b, '.');
      if (nd > e + 1) buf_puts(b, digits + e + 1); else buf_putc(b, '0');
    }
  } else {
    buf_putc(b, digits[0]);
    buf_putc(b, '.');
    buf_puts(b, nd > 1 ? digits + 1 : "0");
    snprintf(tmp, sizeof tmp, "e%d", e);
    buf_puts(b, tmp);
  }
}

static void show_char_esc(Buf *b, int c, char quote) {
  char tmp[8];
  switch (c) {
  case '\n': buf_puts(b, "\\n"); return;
  case '\t': buf_puts(b, "\\t"); return;
  case '\r': buf_puts(b, "\\r"); return;
  case '\\': buf_puts(b, "\\\\"); return;
  }
  if (c == quote) { buf_putc(b, '\\'); buf_putc(b, (char)c); return; }
  if (c < 32 || c >= 127) { snprintf(tmp, sizeof tmp, "\\%d", c); buf_puts(b, tmp); return; }
  buf_putc(b, (char)c);
}

static void show_float(Buf *b, double d, int prec) {
  bool paren = prec > 6 && (d < 0 || signbit(d));
  if (paren) buf_putc(b, '(');
  show_double(b, d);
  if (paren) buf_putc(b, ')');
}

static void show(Buf *b, V v, int prec) {
  FK_STACK_CHECK();
  char tmp[32];
  if (FK_ISINT(v)) {
    intptr_t i = FK_INT(v);
    snprintf(tmp, sizeof tmp, prec > 6 && i < 0 ? "(%ld)" : "%ld", (long)i);
    buf_puts(b, tmp);
    return;
  }
  if (FK_ISCHAR(v)) {
    buf_putc(b, '\'');
    show_char_esc(b, FK_CHAR(v), '\'');
    buf_putc(b, '\'');
    return;
  }
  FkHdr *h = FK_HDR(v);
  if (h->kind == K_FLOAT) {
    show_float(b, FK_FLOATV(v), prec);
    return;
  }
  if (h->kind != K_CON) { buf_puts(b, "<function>"); return; }
  uint32_t id = h->aux;
  if (id == FK_C_NIL) { buf_puts(b, "[]"); return; }
  if (id == FK_C_CONS) {
    if (FK_ISCHAR(FK_FIELD(v, 0))) {
      buf_putc(b, '"');
      for (; FK_CONID(v) == FK_C_CONS; v = FK_FIELD(v, 1)) show_char_esc(b, FK_CHAR(FK_FIELD(v, 0)), '"');
      buf_putc(b, '"');
      return;
    }
    buf_putc(b, '[');
    for (bool first = true; FK_CONID(v) == FK_C_CONS; v = FK_FIELD(v, 1), first = false) {
      if (!first) buf_putc(b, ',');
      show(b, FK_FIELD(v, 0), 0);
    }
    buf_putc(b, ']');
    return;
  }
  if (id >= FK_C_TUP2 && id < FK_C_TUP2 + FK_MAXTUPLE - 1) {
    buf_putc(b, '(');
    for (int i = 0; i < h->n; i++) {
      if (i) buf_putc(b, ',');
      show(b, FK_FIELD(v, i), 0);
    }
    buf_putc(b, ')');
    return;
  }
  if (h->n == 0) { buf_puts(b, fk_cons[id].name); return; }
  const char *const *ls = fk_cons[id].labels;
  if (ls) { /* Rect {w = 1.0, h = 2.0} */
    if (prec > 10) buf_putc(b, '(');
    buf_puts(b, fk_cons[id].name);
    for (int i = 0; i < h->n; i++) {
      buf_puts(b, i ? ", " : " {");
      buf_puts(b, ls[i]);
      buf_puts(b, " = ");
      show(b, FK_FIELD(v, i), 0);
    }
    buf_putc(b, '}');
    if (prec > 10) buf_putc(b, ')');
    return;
  }
  if (prec > 10) buf_putc(b, '(');
  buf_puts(b, fk_cons[id].name);
  for (int i = 0; i < h->n; i++) {
    buf_putc(b, ' ');
    show(b, FK_FIELD(v, i), 11);
  }
  if (prec > 10) buf_putc(b, ')');
}

char *fk_show_cstr(V v) {
  Buf b = {0};
  buf_putc(&b, 0);
  b.n = 0;
  show(&b, v, 0);
  return b.s;
}

/* ------------------------------------------------------------- compare */

static bool is_num(V v) { return FK_ISINT(v) || is_float(v); }
static double num_d(V v) { return FK_ISINT(v) ? (double)FK_INT(v) : FK_FLOATV(v); }

/* Structural ordering; lists and other right-nested data loop on the
   last field so long lists do not deepen the C stack. */
int fk_compare(V a, V b) {
  FK_STACK_CHECK();
  for (;;) {
    if (a == b) return 0;
    if (FK_ISINT(a) && FK_ISINT(b)) return FK_INT(a) < FK_INT(b) ? -1 : 1;
    if (FK_ISCHAR(a) && FK_ISCHAR(b)) return FK_CHAR(a) < FK_CHAR(b) ? -1 : 1;
    if (is_num(a) && is_num(b)) {
      double x = num_d(a), y = num_d(b);
      return x < y ? -1 : x > y ? 1 : 0;
    }
    if (!FK_ISPTR(a) || !FK_ISPTR(b) || FK_HDR(a)->kind != K_CON || FK_HDR(b)->kind != K_CON)
      fk_error("cannot compare functions");
    uint32_t ia = FK_CONID(a), ib = FK_CONID(b);
    if (ia != ib) return ia < ib ? -1 : 1;
    int n = FK_HDR(a)->n;
    if (n == 0) return 0;
    for (int i = 0; i < n - 1; i++) {
      int c = fk_compare(FK_FIELD(a, i), FK_FIELD(b, i));
      if (c) return c;
    }
    a = FK_FIELD(a, n - 1);
    b = FK_FIELD(b, n - 1);
  }
}

int fk_num_eq_int(V v, long long i) {
  if (FK_ISINT(v)) return FK_INT(v) == i;
  if (!is_float(v)) fk_error("numeric literal pattern on a type other than Int or Float");
  return FK_FLOATV(v) == (double)i;
}

/* ------------------------------------------------------------ primitives */

/* Typed primitives as inline functions on tagged words. Generated C calls
   the fk_i_ forms directly, so the C compiler inlines them; the fk_p_
   wrappers serve the interpreter and function values. Int arithmetic
   wraps (unsigned in C, so defined). Tagged Ints and Chars are ordered
   like their values, so they compare as words. Float follows IEEE:
   NaN /= NaN. */
static inline int64_t fk_s_absi(int64_t x) { return x < 0 ? (int64_t)(0 - (uint64_t)x) : x; }

#define I2(name, expr)                                                         \
  static inline V fk_i_##name(V x, V y) { return expr; }                     \
  V fk_p_##name(V self, V *a) { return fk_i_##name(a[0], a[1]); }
#define I1(name, expr)                                                         \
  static inline V fk_i_##name(V x) { return expr; }                          \
  V fk_p_##name(V self, V *a) { return fk_i_##name(a[0]); }
#define IWRAP(op) FK_MKINT((intptr_t)((uintptr_t)FK_INT(x) op (uintptr_t)FK_INT(y)))

I2(addi, IWRAP(+))
I2(subi, IWRAP(-))
I2(muli, IWRAP(*))
I2(addf, fk_float(FK_FLOATV(x) + FK_FLOATV(y)))
I2(subf, fk_float(FK_FLOATV(x) - FK_FLOATV(y)))
I2(mulf, fk_float(FK_FLOATV(x) * FK_FLOATV(y)))
I1(negi, FK_MKINT((intptr_t)(0 - (uintptr_t)FK_INT(x))))
I1(negf, fk_float(-FK_FLOATV(x)))
I1(absi, FK_MKINT(fk_s_absi(FK_INT(x))))
I1(absf, fk_float(fabs(FK_FLOATV(x))))
I2(fdiv, fk_float(FK_FLOATV(x) / FK_FLOATV(y)))

/* Comparisons also have a C truth value form, fk_b_NAME, for conditions. */
#define B2(name, expr)                                                         \
  static inline bool fk_b_##name(V x, V y) { return expr; }                  \
  I2(name, FK_BOOL(fk_b_##name(x, y)))

B2(eqw, x == y) /* Int, Char */
B2(new, x != y)
B2(ltw, (intptr_t)x < (intptr_t)y)
B2(lew, (intptr_t)x <= (intptr_t)y)
B2(gtw, (intptr_t)x > (intptr_t)y)
B2(gew, (intptr_t)x >= (intptr_t)y)
B2(eqf, FK_FLOATV(x) == FK_FLOATV(y))
B2(nef, FK_FLOATV(x) != FK_FLOATV(y))
B2(ltf, FK_FLOATV(x) < FK_FLOATV(y))
B2(lef, FK_FLOATV(x) <= FK_FLOATV(y))
B2(gtf, FK_FLOATV(x) > FK_FLOATV(y))
B2(gef, FK_FLOATV(x) >= FK_FLOATV(y))

_Noreturn static void div_zero(void) { fk_error("divide by zero"); }

/* Scalar forms, fk_s_NAME, for typed generated code. div and mod round
   toward negative infinity, quot and rem toward zero. INT64_MIN / -1
   wraps instead of trapping. */
static inline int64_t fk_s_div(int64_t x, int64_t y) {
  if (y == 0) div_zero();
  if (y == -1) return (int64_t)(0 - (uint64_t)x);
  int64_t q = x / y;
  if (x % y != 0 && ((x < 0) != (y < 0))) q--;
  return q;
}
static inline int64_t fk_s_mod(int64_t x, int64_t y) {
  if (y == 0) div_zero();
  if (y == -1) return 0;
  int64_t r = x % y;
  if (r != 0 && ((r < 0) != (y < 0))) r += y;
  return r;
}
static inline int64_t fk_s_quot(int64_t x, int64_t y) {
  if (y == 0) div_zero();
  return y == -1 ? (int64_t)(0 - (uint64_t)x) : x / y;
}
static inline int64_t fk_s_rem(int64_t x, int64_t y) {
  if (y == 0) div_zero();
  return y == -1 ? 0 : x % y;
}
static inline int32_t fk_s_chr(int64_t i) {
  if (i < 0 || i > 255) fk_error("chr: %lld out of range", (long long)i);
  return (int32_t)i;
}
static inline V fk_i_div(V a, V b) { return FK_MKINT(fk_s_div(FK_INT(a), FK_INT(b))); }
static inline V fk_i_mod(V a, V b) { return FK_MKINT(fk_s_mod(FK_INT(a), FK_INT(b))); }
static inline V fk_i_quot(V a, V b) { return FK_MKINT(fk_s_quot(FK_INT(a), FK_INT(b))); }
static inline V fk_i_rem(V a, V b) { return FK_MKINT(fk_s_rem(FK_INT(a), FK_INT(b))); }
V fk_p_div(V self, V *a) { return fk_i_div(a[0], a[1]); }
V fk_p_mod(V self, V *a) { return fk_i_mod(a[0], a[1]); }
V fk_p_quot(V self, V *a) { return fk_i_quot(a[0], a[1]); }
V fk_p_rem(V self, V *a) { return fk_i_rem(a[0], a[1]); }

V fk_p_tofloat(V self, V *a) { return fk_float((double)FK_INT(a[0])); }

static V to_int(double d) {
  if (!(d > -4.6e18 && d < 4.6e18)) fk_error("float out of Int range");
  return FK_MKINT((intptr_t)d);
}
V fk_p_floor(V self, V *a) { return to_int(floor(num_d(a[0]))); }
V fk_p_ceiling(V self, V *a) { return to_int(ceil(num_d(a[0]))); }
V fk_p_round(V self, V *a) { return to_int(nearbyint(num_d(a[0]))); } /* ties to even */
V fk_p_truncate(V self, V *a) { return to_int(trunc(num_d(a[0]))); }
V fk_p_sqrt(V self, V *a) { return fk_float(sqrt(num_d(a[0]))); }
V fk_p_exp(V self, V *a) { return fk_float(exp(num_d(a[0]))); }
V fk_p_log(V self, V *a) { return fk_float(log(num_d(a[0]))); }
V fk_p_sin(V self, V *a) { return fk_float(sin(num_d(a[0]))); }
V fk_p_cos(V self, V *a) { return fk_float(cos(num_d(a[0]))); }
V fk_p_pow(V self, V *a) { return fk_float(pow(num_d(a[0]), num_d(a[1]))); }

V fk_p_ord(V self, V *a) { return FK_MKINT(FK_CHAR(a[0])); }
V fk_p_chr(V self, V *a) { return FK_MKCHAR(fk_s_chr(FK_INT(a[0]))); }

V fk_p_show(V self, V *a) {
  char *s = fk_show_cstr(a[0]);
  V r = fk_string(s, strlen(s));
  free(s);
  return r;
}

V fk_p_print(V self, V *a) {
  char *s = fk_show_cstr(a[0]);
  puts(s);
  free(s);
  return fk_unit;
}

/* show for [Char]: quoted, also when empty. */
V fk_p_showstr(V self, V *a) {
  if (a[0] == fk_nil) return fk_string("\"\"", 2);
  return fk_p_show(self, a);
}

/* Constructor index, for derived Ord. */
V fk_p_contag(V self, V *a) {
  if (!FK_ISPTR(a[0]) || FK_HDR(a[0])->kind != K_CON) fk_error("primConTag: not a constructor");
  return FK_MKINT(FK_CONID(a[0]));
}

V fk_p_putstr(V self, V *a) {
  size_t n;
  char *s = fk_cstring(a[0], &n);
  fwrite(s, 1, n, stdout);
  free(s);
  return fk_unit;
}

V fk_p_putstrln(V self, V *a) {
  fk_p_putstr(self, a);
  putchar('\n');
  return fk_unit;
}

V fk_p_getline(V self, V *a) {
  Buf b = {0};
  int c;
  fflush(stdout);
  buf_putc(&b, 0);
  b.n = 0;
  while ((c = getchar()) != EOF && c != '\n') buf_putc(&b, (char)c);
  if (c == EOF && b.n == 0) { free(b.s); fk_error("getLine: end of file"); }
  V r = fk_string(b.s, b.n);
  free(b.s);
  return r;
}

V fk_p_readint(V self, V *a) {
  char *s = fk_cstring(a[0], NULL), *end;
  errno = 0;
  long long i = strtoll(s, &end, 10);
  while (isspace((unsigned char)*end)) end++;
  bool bad = end == s || *end || errno;
  if (bad) { fk_error("readInt: no parse: \"%s\"", s); }
  free(s);
  return FK_MKINT(i);
}

V fk_p_readfloat(V self, V *a) {
  char *s = fk_cstring(a[0], NULL), *end;
  double d = strtod(s, &end);
  while (isspace((unsigned char)*end)) end++;
  if (end == s || *end) fk_error("readFloat: no parse: \"%s\"", s);
  free(s);
  return fk_float(d);
}

V fk_p_error(V self, V *a) {
  char *s = fk_cstring(a[0], NULL);
  fk_error("%s", s);
}

V fk_p_then(V self, V *a) { return a[1]; }

const FkPrim fk_prims[] = {
#define P(c, name, ar, ty) {name, ar, fk_p_##c},
#include "prims.def"
#undef P
};
const int fk_nprims = sizeof fk_prims / sizeof fk_prims[0];
static V prim_values[sizeof fk_prims / sizeof fk_prims[0]];

V fk_prim_value(int i) {
  if (!prim_values[i]) {
    prim_values[i] = static_obj(3, K_CLOSURE, 0, (uint32_t)fk_prims[i].arity);
    FK_W(prim_values[i])[1] = (V)fk_prims[i].code;
  }
  return prim_values[i];
}

/* ------------------------------------------------------------------ init */

void fk_init(void) {
  static const char *tuples[] = {"(,)", "(,,)", "(,,,)", "(,,,,)", "(,,,,,)", "(,,,,,,)"};
  if (fk_ncons) return;
  fk_register_con("False", 0);
  fk_register_con("True", 0);
  fk_register_con("()", 0);
  fk_register_con("[]", 0);
  fk_register_con(":", 2);
  for (int i = 0; i < FK_MAXTUPLE - 1; i++) fk_register_con(tuples[i], i + 2);
  fk_false = fk_cons[FK_C_FALSE].nullary;
  fk_true = fk_cons[FK_C_TRUE].nullary;
  fk_unit = fk_cons[FK_C_UNIT].nullary;
  fk_nil = fk_cons[FK_C_NIL].nullary;
}

typedef struct { void (*fn)(void *); void *arg; } Task;

/* Generated code has no stack check: overflowing reaches the guard
   region below the stack and faults. The handler runs on its own stack. */
static char *guard_lo, *guard_hi;

static void on_fault(int sig, siginfo_t *si, void *ctx) {
  char *a = si->si_addr;
  if (a >= guard_lo && a < guard_hi) {
    static const char msg[] = "runtime error: stack overflow\n";
    fflush(stdout); /* not async-signal-safe, but the process exits next */
    if (write(2, msg, sizeof msg - 1) < 0) {}
    _exit(1);
  }
  signal(sig, SIG_DFL); /* a real fault: let it crash */
}

static void *task_main(void *p) {
  Task *t = p;
  stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16};
  struct sigaction sa = {0};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  if (ss.ss_sp && sigaltstack(&ss, NULL) == 0) {
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
  }
  t->fn(t->arg);
  fflush(stdout);
  return NULL;
}

/* Deep non-tail recursion (map, foldr) is normal in this language, so
   funk code runs on a thread with a 4 GiB stack. MAP_NORESERVE commits
   pages only as the stack grows; the size halves if the mapping fails.
   The interpreter and the runtime check the stack and report overflow as
   an error the REPL recovers from; compiled code relies on the guard. */
int fk_run(void (*fn)(void *), void *arg) {
  size_t size = (size_t)4 << 30, guard = (size_t)16 << 20; /* larger than any frame */
  fk_init();
  char *mem = MAP_FAILED;
  while (size >= ((size_t)8 << 20)) {
    mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem != MAP_FAILED) break;
    size /= 2;
  }
  if (mem == MAP_FAILED) { perror("funk: mmap stack"); return 1; }
  mprotect(mem, guard, PROT_NONE);
  guard_lo = mem;
  guard_hi = mem + guard;
  fk_stack_base = mem + size;
  fk_stack_limit = mem + guard + ((size_t)1 << 20);
  pthread_attr_t attr;
  pthread_t th;
  Task t = {fn, arg};
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, mem, size);
  if (pthread_create(&th, &attr, task_main, &t)) { perror("funk: pthread_create"); return 1; }
  pthread_join(th, NULL);
  return 0;
}
