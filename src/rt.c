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
#include <sched.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#else
#include <semaphore.h>
#endif

/* ---------------------------------------------------------------- errors */

jmp_buf *fk_error_jmp;
char *fk_stack_limit;
static char *fk_stack_base;

bool fk_on_funk_thread(void);

_Noreturn void fk_error(const char *fmt, ...) {
  va_list ap;
  fflush(stdout);
  fputs("runtime error: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  if (!fk_on_funk_thread()) abort(); /* a foreign export on another thread: funk's handlers are not on this stack */
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

typedef struct {
  size_t words, nobj;
  char *objs;
  uint8_t *marks;
  uint64_t recip;
  size_t next_unswept; /* the next unswept chunk of this size class, plus one; 0 ends the list */
  size_t nmarked;      /* objects marked this collection */
} Chunk;
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
static size_t alloc_since_gc, gc_threshold = (size_t)128 << 20; /* gc_min below */
static V **mstack;
static size_t msp, mcap;
static V *stack_copy; /* the stack when the cycle started: see snapshot_stack */
static size_t stack_words, stack_cap, stack_scanned;
static size_t page_size;
char *fk_snap_lo; /* the lowest read-only page of a cycle's stack snapshot, or NULL */
static char *snap_hi, *snap_next; /* above the read-only pages; the next to scan */
static char *snap_area;                     /* reserved: copies of the pages written while read-only */
static uint8_t *snap_copied;                /* by page */
static size_t snap_pages;
int fk_callback_depth; /* direct callbacks running: C frames below them may be written by the kernel */
static size_t live_bytes;  /* marked this collection */
static size_t black_bytes; /* of which allocated during the cycle: mostly garbage by its end */

/* Lazy sweeping (realtime.md, R1): a collection only marks. Each size
   class keeps a list of unswept chunks, in address order, and allocation
   sweeps the next one when its free list is empty. */
static size_t unswept[MAX_SMALL + 1]; /* a chunk index plus one */

/* Incremental marking (realtime.md, R2), snapshot at the beginning: a
   cycle starts with a pause that scans the stack and the roots, then marks
   in steps of at most STEP_MS, one per STEP_BYTES of allocation. Objects
   allocated meanwhile are marked at once. A write that overwrites a heap
   pointer shades the old value first (FK_SHADE), so everything reachable
   when the cycle started is marked. Nothing is freed until a cycle ends,
   so its allocation takes fresh chunks: 256 KiB, not 1 MiB, cut bst's
   resident set from 207 to 160 MB and its time by 4%. */
#define STEP_BYTES ((size_t)256 << 10)
#define STEP_MS 0.4
int fk_marking;
/* FUNK_GCSTRESS=1, for testing: a cycle 64 KiB after the last one ends,
   steps of 4096 objects every 16 KiB, never finished early, and a check at
   each cycle's end that every object reachable then is marked. Many short
   cycles start often, inside the windows where a missing barrier loses an
   object; tests/run/gcbarrier.fk hits them. */
static bool gcstress;
static size_t step_bytes = STEP_BYTES, gc_min = (size_t)128 << 20;
static size_t cycle_start_threshold; /* gc_threshold when the cycle started */
static size_t next_step; /* while marking, gc_threshold is 0, so every allocation takes alloc_slow, which marks it */

/* FUNK_GCSTATS=1: each cycle's pauses, and a summary at exit (R0). */
static bool gcstats;
static size_t gc_count, lazy_chunks, forced, cycle_steps;
static double pause_total, pause_max, lazy_total, lazy_max, start_max, step_max, finish_max, sweep_step_max;
static double cycle_start, cycle_step_max, cycle_mark;

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void gc_summary(void) {
  fprintf(stderr, "gc: %zu cycles, pause max %.2f ms (start %.2f, mark step %.2f, finish %.2f, sweep step %.2f), total %.2f ms, %zu forced; "
          "lazy sweep %zu chunks, max %.3f ms, total %.2f ms\n",
          gc_count, pause_max, start_max, step_max, finish_max, sweep_step_max, pause_total, forced, lazy_chunks, lazy_max, lazy_total);
}

static void pause_done(double t, double *kind_max) {
  pause_total += t;
  if (t > pause_max) pause_max = t;
  if (t > *kind_max) *kind_max = t;
}

void fk_add_roots(V **base, size_t *count) {
  roots = rt_realloc(roots, (nroots + 1) * sizeof *roots);
  roots[nroots++] = (Root){base, count};
}

static void push_free(size_t words, V *o) {
  o[1] = (V)freelist[words];
  freelist[words] = o;
}

/* A fresh chunk is allocated by bumping a pointer, not through a free list
   built in advance, which wrote every object once before its use; so is a
   chunk that sweeping finds with nothing marked, without touching its dead
   objects. find_obj treats the unallocated rest of a bump region as free,
   since a reused chunk's still holds dead headers. Sweep ends the bump
   regions; their unallocated objects then go on the free lists. */
static V *bump[MAX_SMALL + 1], *bump_end[MAX_SMALL + 1];

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
  bump[words] = (V *)c->objs; /* allocated in address order, each word written once */
  bump_end[words] = (V *)(c->objs + nobj * objbytes);
}

/* Map a candidate word to the object containing it, or NULL. */
static V *find_obj(uintptr_t w, uint8_t **mark, size_t *words) {
  if (w >= heap_lo && w < heap_hi) {
    Chunk *c = (Chunk *)(w & ~(uintptr_t)(CHUNK_SIZE - 1));
    if (w < (uintptr_t)c->objs) return NULL;
    size_t i = (size_t)(((uint64_t)(w - (uintptr_t)c->objs) * c->recip) >> 32);
    if (i >= c->nobj) return NULL;
    V *o = (V *)(c->objs + i * c->words * sizeof(V));
    if (((FkHdr *)o)->kind == K_FREE || (o >= bump[c->words] && o < bump_end[c->words])) return NULL;
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
  live_bytes += words * sizeof(V);
  if (w >= heap_lo && w < heap_hi) ((Chunk *)(w & ~(uintptr_t)(CHUNK_SIZE - 1)))->nmarked++;
  int k = ((FkHdr *)o)->kind;
  if (k == K_FLOAT || k == K_STR || k == K_INT64 || k == K_PTR) return; /* a number, bytes or a C address */
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

static bool scan_snapshot(double end, bool timed);

/* Marks for up to budget_ms, checking the clock every 256 objects; true
   when the mark stack is empty. */
static bool drain(double budget_ms) {
  double end = now_ms() + budget_ms;
  while (stack_scanned < stack_words) { /* the stack's snapshot first, 8 Ki words at a time */
    size_t k = stack_words - stack_scanned < 8192 ? stack_words - stack_scanned : 8192;
    scan_range(stack_copy + stack_scanned, stack_copy + stack_scanned + k);
    stack_scanned += k;
    if (budget_ms < INFINITY && (gcstress || now_ms() > end)) return false; /* stress: a chunk per step, reproducibly */
  }
  if (!scan_snapshot(end, budget_ms < INFINITY)) return false;
  for (int n = 0; msp;) {
    size_t words = (size_t)(uintptr_t)mstack[--msp];
    V *o = mstack[--msp];
    scan_range(o + 1, o + words); /* every payload word, conservatively */
    if (++n == (gcstress ? 4096 : 256)) {
      if (gcstress && budget_ms < INFINITY) return false; /* steps of a fixed size, for reproducible tests */
      n = 0;
      if (now_ms() > end) return false;
    }
  }
  return true;
}

void fk_shade(V old) { mark_word(old); }

/* An object allocated while marking is marked: it is live at the cycle's
   end, and its fields point to objects that are new or reachable when the
   cycle started. */
static __attribute__((noinline)) void alloc_black(V *o) {
  uint8_t *m;
  size_t words;
  if (!find_obj((uintptr_t)o, &m, &words) || *m) return;
  *m = 1;
  live_bytes += words * sizeof(V);
  black_bytes += words * sizeof(V);
  if ((uintptr_t)o >= heap_lo && (uintptr_t)o < heap_hi) ((Chunk *)((uintptr_t)o & ~(uintptr_t)(CHUNK_SIZE - 1)))->nmarked++;
}

/* A cycle's snapshot of the stack (realtime.md): the start pause copies
   the words up to the next page boundary and makes the older pages read
   only; a write to one of them copies the page first (on_fault). Steps
   scan each page, from its copy if it was written, and make it writable
   again. The pause is then an mprotect, not a scan of the whole stack.
   Nothing is freed before the cycle ends, so what the snapshot points to
   stays in place. While a direct callback runs, the C frames below it may
   pass stack memory to the kernel, which a read-only page would fail with
   EFAULT; then the whole stack is copied instead. */
static void copy_words(V *from, size_t n) {
  if (n > stack_cap) {
    free(stack_copy);
    stack_cap = n + n / 4;
    stack_copy = malloc(stack_cap * sizeof(V));
    if (!stack_copy) { fputs("funk: out of memory\n", stderr); exit(1); }
  }
  memcpy(stack_copy, from, n * sizeof(V));
  stack_words = n;
  stack_scanned = 0;
}

static char *guard_lo, *guard_hi; /* the stack's guard region, set by fk_run */

static char *snapshot_sp; /* for the check's report */
static char *stack_top; /* task_main's frame: above it, the thread's own data (glibc keeps its TCB and TLS there) */

static void copy_more(V *from, size_t n) { /* appended to the copy */
  size_t at = stack_words;
  if (at + n > stack_cap) {
    stack_cap = (at + n) + (at + n) / 4;
    stack_copy = realloc(stack_copy, stack_cap * sizeof(V));
    if (!stack_copy) { fputs("funk: out of memory\n", stderr); exit(1); }
  }
  memcpy(stack_copy + at, from, n * sizeof(V));
  stack_words = at + n;
}

static __attribute__((noinline)) void snapshot_stack(void) {
  V *sp = (V *)__builtin_frame_address(0);
  snapshot_sp = (char *)sp;
  if (!page_size) page_size = (size_t)sysconf(_SC_PAGESIZE);
  if (gcstress) { /* the dead stack below holds old words, which new frames' unset slots would show the check */
    char *dead = (char *)(((uintptr_t)sp - 2 * page_size) & ~(uintptr_t)(page_size - 1));
    /* fresh pages, which read as zero: macOS's MADV_DONTNEED keeps the old contents */
    if (dead > guard_hi &&
        mmap(guard_hi, (size_t)(dead - guard_hi), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED) {
      fputs("funk: cannot clear the stack for FUNK_GCSTRESS\n", stderr);
      exit(1);
    }
    memset(dead, 0, (size_t)((char *)sp - 1024 - dead));
  }
  char *lo = (char *)(((uintptr_t)sp + page_size - 1) & ~(uintptr_t)(page_size - 1)),
       *hi = (char *)((uintptr_t)stack_top & ~(uintptr_t)(page_size - 1)); /* below the thread's data, which the kernel writes */
  if (fk_callback_depth || lo >= hi) {
    copy_words(sp, (size_t)((V *)fk_stack_base - sp));
    return;
  }
  size_t pages = (size_t)(hi - lo) / page_size;
  if (pages > snap_pages) { /* address space for copies, touched only where a page is written */
    if (snap_area) munmap(snap_area, snap_pages * page_size);
    snap_area = mmap(NULL, pages * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    free(snap_copied);
    snap_copied = malloc(pages);
    if (snap_area == MAP_FAILED || !snap_copied) { fputs("funk: out of memory\n", stderr); exit(1); }
    snap_pages = pages;
  }
  memset(snap_copied, 0, pages);
  copy_words(sp, (size_t)((V *)lo - sp));
  copy_more((V *)hi, (size_t)((V *)fk_stack_base - (V *)hi)); /* the frames and data above the read-only pages */
  if (mprotect(lo, (size_t)(hi - lo), PROT_READ) != 0) { /* not expected; a copy is always correct */
    copy_words(sp, (size_t)((V *)fk_stack_base - sp));
    return;
  }
  fk_snap_lo = snap_next = lo;
  snap_hi = hi;
}

/* Before a foreign call on funk's thread: the C code's frames go below the
   current frame, into snapshot pages funk returned above. The kernel
   cannot write into a read-only one, so the pages from the scan cursor to
   here are kept and made writable now. */
void fk_snapshot_release(char *sp) {
  if (!fk_snap_lo || !fk_on_funk_thread()) return;
  char *top = (char *)(((uintptr_t)sp & ~(uintptr_t)(page_size - 1)) + page_size);
  if (top > snap_hi) top = snap_hi;
  if (top <= snap_next) return;
  for (char *pg = snap_next; pg < top; pg += page_size) {
    size_t i = (size_t)(pg - fk_snap_lo) / page_size;
    if (!snap_copied[i]) {
      memcpy(snap_area + i * page_size, pg, page_size);
      snap_copied[i] = 1;
    }
  }
  mprotect(snap_next, (size_t)(top - snap_next), PROT_READ | PROT_WRITE);
}

/* Scans the snapshot's pages for up to the step's budget; true when done. */
static bool scan_snapshot(double end, bool timed) {
  while (snap_next < snap_hi) {
    char *from = snap_next;
    for (int k = 0; k < 1 && snap_next < snap_hi; k++, snap_next += page_size) { /* a page, then the clock */
      size_t i = (size_t)(snap_next - fk_snap_lo) / page_size;
      V *p = (V *)(snap_copied[i] ? snap_area + i * page_size : snap_next);
      scan_range(p, p + page_size / sizeof(V));
    }
    mprotect(from, (size_t)(snap_next - from), PROT_READ | PROT_WRITE);
    if (timed && (gcstress || now_ms() > end)) break;
  }
  if (snap_next < snap_hi) return false;
  fk_snap_lo = snap_hi = snap_next = NULL;
  return true;
}

static Chunk *chunk_at(size_t i) { return (Chunk *)(heap_base + i * CHUNK_SIZE); }

/* Frees a chunk's unmarked objects onto its free list, lowest address
   first, and clears its marks. */
static void sweep_chunk(Chunk *c) {
  size_t bytes = c->words * sizeof(V);
  if (c->nmarked == 0 && bump[c->words] >= bump_end[c->words]) { /* all dead: the next bump region */
    bump[c->words] = (V *)c->objs;
    bump_end[c->words] = (V *)(c->objs + c->nobj * bytes);
    return;
  }
  bool full = c->nmarked == c->nobj; /* nothing to free: only the marks to clear */
  c->nmarked = 0;
  if (full) {
    memset(c->marks, 0, c->nobj);
    return;
  }
  for (size_t i = c->nobj; i-- > 0;) {
    V *o = (V *)(c->objs + i * bytes);
    if (((FkHdr *)o)->kind != K_FREE) {
      if (c->marks[i]) { c->marks[i] = 0; continue; }
      o[0] = 0; /* K_FREE; constructors overwrite every payload word */
    }
    push_free(c->words, o);
  }
}

/* Sweeps unswept chunks of a size class until its free list has an object. */
static void sweep_lazily(size_t words) {
  double t0 = gcstats ? now_ms() : 0;
  while (!freelist[words] && bump[words] >= bump_end[words] && unswept[words]) {
    Chunk *c = chunk_at(unswept[words] - 1);
    unswept[words] = c->next_unswept;
    sweep_chunk(c);
    lazy_chunks++;
  }
  if (gcstats) {
    double t = now_ms() - t0;
    lazy_total += t;
    if (t > lazy_max) lazy_max = t;
  }
}

/* A chunk still unswept when a collection starts keeps last cycle's marks;
   clearing them lets this cycle's sweep free what died since. */
static void clear_unswept_marks(void) {
  for (size_t w = 0; w <= MAX_SMALL; w++)
    for (size_t i = unswept[w]; i; i = chunk_at(i - 1)->next_unswept) {
      memset(chunk_at(i - 1)->marks, 0, chunk_at(i - 1)->nobj);
      chunk_at(i - 1)->nmarked = 0;
    }
}

/* After marking: every chunk becomes unswept, and dead large objects are
   freed now, since they are few. */
static void sweep(void) {
  memset(freelist, 0, sizeof freelist);
  memset(bump, 0, sizeof bump);
  memset(bump_end, 0, sizeof bump_end);
  memset(unswept, 0, sizeof unswept);
  fk_marking = 0;
  for (size_t ci = nchunks; ci-- > 0;) { /* pushed in reverse: each list is in address order */
    Chunk *c = chunk_at(ci);
    c->next_unswept = unswept[c->words];
    unswept[c->words] = ci + 1;
  }
  size_t live = live_bytes - black_bytes, j = 0; /* reachable when the cycle started, large objects too */
  large_lo = UINTPTR_MAX;
  large_hi = 0;
  for (size_t i = 0; i < nlarge; i++) {
    if (!larges[i].mark) { free(larges[i].p); continue; }
    larges[i].mark = 0;
    larges[j++] = larges[i];
    if ((uintptr_t)larges[i].p < large_lo) large_lo = (uintptr_t)larges[i].p;
    uintptr_t end = (uintptr_t)(larges[i].p + larges[i].words);
    if (end > large_hi) large_hi = end;
  }
  nlarge = j;
  gc_threshold = 2 * live > gc_min && !gcstress ? 2 * live : gc_min;
}

/* The cycle's first pause: the snapshot of the stack and the roots. */
static void gc_start(void) {
  jmp_buf regs;
  setjmp(regs); /* spill callee-saved registers into this frame */
  __asm__ volatile("" : : "r"(&regs) : "memory");
  double t0 = gcstats ? now_ms() : 0;
  clear_unswept_marks();
  live_bytes = black_bytes = 0;
  fk_marking = 1;
  cycle_start_threshold = gc_threshold;
  snapshot_stack();
  for (size_t i = 0; i < nroots; i++) scan_range(*roots[i].base, *roots[i].base + *roots[i].count);
  if (gcstats) {
    cycle_start = now_ms() - t0;
    cycle_steps = 0;
    cycle_step_max = cycle_mark = 0;
    pause_done(cycle_start, &start_max);
  }
}

/* FUNK_GCSTRESS: every object reachable now must be marked. The check
   marks what it visits 2, which the sweep reads as marked. */
static V *verify_from, *verify_slot; /* the object, or the root, being scanned: for the report */

static void verify_word(uintptr_t w, V ***stack, size_t *n, size_t *cap) {
  uint8_t *m;
  size_t words;
  V *o = FK_ISPTR(w) ? find_obj(w, &m, &words) : NULL;
  if (!o || *m == 2) return;
  if (!*m) {
    fprintf(stderr, "funk: gc check: reachable object %p (kind %d) is unmarked at the end of a cycle\n", (void *)o, ((FkHdr *)o)->kind);
    if (verify_from) {
      uint8_t *pm;
      size_t pw;
      find_obj((uintptr_t)verify_from, &pm, &pw);
      fprintf(stderr, "  in field %ld of %p (kind %d, mark %d)\n", (long)(verify_slot - verify_from - 1), (void *)verify_from,
              ((FkHdr *)verify_from)->kind, *pm);
    } else if ((char *)verify_slot >= (char *)fk_stack_base - (1LL << 32) && (char *)verify_slot < fk_stack_base) {
      fprintf(stderr, "  in the stack at %p, %s the cycle's start at %p\n", (void *)verify_slot,
              (char *)verify_slot < snapshot_sp ? "deeper than" : "in a frame from before", snapshot_sp);
    } else {
      fprintf(stderr, "  in a root table at %p\n", (void *)verify_slot);
    }
    abort();
  }
  *m = 2;
  int k = ((FkHdr *)o)->kind;
  if (k == K_FLOAT || k == K_STR || k == K_INT64 || k == K_PTR) return;
  if (*n + 2 > *cap) *stack = rt_realloc(*stack, (*cap = *cap ? 2 * *cap : 4096) * sizeof **stack);
  (*stack)[(*n)++] = o;
  (*stack)[(*n)++] = (V *)(uintptr_t)words;
}

static __attribute__((noinline)) void verify_marks(void) {
  jmp_buf regs;
  setjmp(regs);
  __asm__ volatile("" : : "r"(&regs) : "memory");
  static V **st;
  static size_t cap;
  size_t n = 0;
  verify_from = NULL;
  for (V *p = (V *)__builtin_frame_address(0); p < (V *)fk_stack_base; p++) verify_word(*(verify_slot = p), &st, &n, &cap);
  for (size_t i = 0; i < nroots; i++)
    for (V *p = *roots[i].base; p < *roots[i].base + *roots[i].count; p++) verify_word(*(verify_slot = p), &st, &n, &cap);
  while (n) {
    size_t words = (size_t)(uintptr_t)st[--n];
    V *o = st[--n];
    verify_from = o;
    for (V *p = o + 1; p < o + words; p++) verify_word(*(verify_slot = p), &st, &n, &cap);
  }
}

/* The last pause: unswept lists for every chunk, and large objects swept. */
static void gc_finish(bool was_forced) {
  double t0 = gcstats ? now_ms() : 0;
  if (gcstress) verify_marks();
  sweep();
  alloc_since_gc = 0;
  forced += was_forced;
  if (gcstats) {
    double t = now_ms() - t0;
    pause_done(t, &finish_max);
    gc_count++;
    fprintf(stderr, "gc %zu: start %.2f ms, %zu steps (max %.2f ms), finish %.2f ms, mark %.2f ms in all%s, live %.1f MiB, heap %zu MiB\n", gc_count,
            cycle_start, cycle_steps, cycle_step_max, t, cycle_mark, was_forced ? ", forced" : "", (double)(live_bytes - black_bytes) / (1 << 20),
            nchunks * (CHUNK_SIZE >> 20));
  }
}

/* A step of marking, or the cycle's end when marking is done. If the cycle
   has allocated a whole threshold more, marking cannot keep up: it
   finishes in one pause. */
static void gc_step(void) {
  bool force = !gcstress && alloc_since_gc > 2 * cycle_start_threshold;
  double t0 = gcstats ? now_ms() : 0;
  bool done = drain(force ? INFINITY : STEP_MS);
  if (gcstats) {
    double t = now_ms() - t0;
    cycle_steps++;
    cycle_mark += t;
    if (t > cycle_step_max) cycle_step_max = t;
    pause_done(t, &step_max);
  }
  if (done) gc_finish(force);
}

/* Before a cycle marks, sweeping finishes, in steps like marking's: a
   chunk unswept while marking could not be swept until the next cycle, so
   allocation would take fresh chunks and the heap would grow each cycle. */
static bool sweep_step(void) {
  double t0 = now_ms(), end = t0 + STEP_MS - 0.15; /* a chunk takes up to about 0.15 ms */
  bool done = true;
  for (size_t w = 0; w <= MAX_SMALL && done; w++)
    while (unswept[w]) {
      Chunk *c = chunk_at(unswept[w] - 1);
      unswept[w] = c->next_unswept;
      sweep_chunk(c);
      lazy_chunks++;
      if (now_ms() > end) {
        done = false;
        break;
      }
    }
  for (size_t w = 0; w <= MAX_SMALL; w++) done &= !unswept[w];
  if (gcstats) pause_done(now_ms() - t0, &sweep_step_max);
  return done;
}

static void gc_due(void) {
  if (fk_marking) gc_step();
  else if (sweep_step()) gc_start();
  if (fk_marking) {
    gc_threshold = 0;
    next_step = alloc_since_gc + step_bytes;
  } else if (alloc_since_gc >= gc_threshold) {
    gc_threshold = alloc_since_gc + step_bytes; /* the next sweep step */
  }
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
  if (fk_marking ? alloc_since_gc > next_step : alloc_since_gc > gc_threshold) gc_due();
  if (words > MAX_SMALL) {
    o = (V *)alloc_large(words);
  } else {
    if (!freelist[words] && unswept[words] && !fk_marking) sweep_lazily(words); /* marks are partial while marking */
    if ((o = freelist[words])) {
      freelist[words] = (V *)o[1];
      o[1] = 0;
    } else {
      if (bump[words] >= bump_end[words]) new_chunk(words);
      o = bump[words];
      bump[words] += words;
      o[1] = 0; /* a reused chunk's dead word, as on a free-list pop */
    }
  }
  FkHdr *h = (FkHdr *)o;
  h->kind = (uint8_t)kind;
  h->n = (uint16_t)n;
  h->aux = aux;
  if (fk_marking) alloc_black(o);
  return (V)o;
}

/* The common case inline: a small object from a non-empty free list, with
   no collection due. */
static inline __attribute__((always_inline)) V alloc(size_t words, int kind, int n, uint32_t aux) {
  if (words < 2) words = 2;
  alloc_since_gc += words * sizeof(V);
  V *o;
  if (words > MAX_SMALL || alloc_since_gc > gc_threshold) return alloc_slow(words, kind, n, aux);
  if ((o = freelist[words])) {
    freelist[words] = (V *)o[1];
    o[1] = 0;
  } else if (bump[words] < bump_end[words]) {
    o = bump[words];
    bump[words] += words;
    o[1] = 0; /* a reused chunk's dead word, as on a free-list pop */
  } else {
    return alloc_slow(words, kind, n, aux);
  }
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

static const FkNullary *nullary_table;
static uint32_t nullary_n;

void fk_nullary_table(const FkNullary *t, uint32_t n) {
  nullary_table = t;
  nullary_n = n;
}

static V nullary_obj(uint32_t id) {
  if (id < nullary_n && nullary_table[id].h.kind == K_CON && nullary_table[id].h.aux == id) return (V)&nullary_table[id];
  return static_obj(1, K_CON, 0, id);
}

uint32_t fk_register_con(const char *name, int arity) {
  if (fk_ncons == capcons) {
    capcons = capcons ? 2 * capcons : 64;
    fk_cons = rt_realloc(fk_cons, capcons * sizeof *fk_cons);
  }
  uint32_t id = fk_ncons++;
  fk_cons[id] = (FkCon){name, arity, arity == 0 ? nullary_obj(id) : 0, 0, NULL, 0};
  return id;
}

void fk_con_labels(uint32_t id, const char *const *labels) { fk_cons[id].labels = labels; }
void fk_con_raw(uint32_t id, uint32_t mask) { fk_cons[id].raw = mask; }

static bool raw_field(V v, int i) { return i < 32 && (fk_cons[FK_CONID(v)].raw >> i & 1); }

void fk_truncate_cons(uint32_t n) { fk_ncons = n; }

/* A constructor applied as a function value gets boxed arguments. */
static V con_code(V self, V *a) {
  V o = fk_mkcon((uint32_t)(uintptr_t)FK_INFO(self), (int)FK_ARITY(self), a);
  for (int i = 0; i < FK_HDR(o)->n; i++)
    if (raw_field(o, i)) FK_FIELD(o, i) = fk_fraw(FK_FLOATV(a[i]));
  return o;
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

V fk_int64_box(int64_t i) {
  V o = alloc(2, K_INT64, 0, 0);
  ((FkInt64 *)o)->i = i;
  return o;
}

V fk_ptr(const void *p) {
  V o = alloc(2, K_PTR, 0, 0);
  FK_PTRV(o) = (void *)p;
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
static __attribute__((noinline)) void shade_free(V t) {
  for (int i = 0; i < FK_HDR(t)->n; i++) fk_shade(FK_FREE(t, i));
}

V fk_force(V t) {
  if (FK_W(t)[1]) {
    V v = FK_CODE(t)(t, NULL);
    FK_W(t)[1] = 0;
    FK_W(t)[2] = v;
    if (fk_marking) shade_free(t); /* v may hold them */
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

static size_t str_words(size_t n) { return 1 + (n + 1 + sizeof(V) - 1) / sizeof(V); }

V fk_str(const char *s, size_t n) {
  if (n > UINT32_MAX) fk_error("string too long");
  V o = alloc(str_words(n), K_STR, 0, (uint32_t)n);
  memcpy(FK_STRBYTES(o), s, n);
  FK_STRBYTES(o)[n] = 0;
  return o;
}

V fk_str_static(const char *s, size_t n) {
  V o = static_obj(str_words(n), K_STR, 0, (uint32_t)n);
  memcpy(FK_STRBYTES(o), s, n);
  return o;
}

bool fk_str_eq_lit(V s, const char *lit, size_t n) { return FK_STRLEN(s) == n && memcmp(FK_STRBYTES(s), lit, n) == 0; }

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

char *fk_cstring(V s, size_t *len) {
  size_t n = FK_STRLEN(s);
  char *c = rt_realloc(NULL, n + 1);
  memcpy(c, FK_STRBYTES(s), n + 1);
  if (len) *len = n;
  return c;
}

/* ------------------------------------------------------------------ show */

static bool is_float(V v) { return FK_ISPTR(v) && FK_HDR(v)->kind == K_FLOAT; }

/* Haskell's format: shortest round-trip digits, fixed notation for
   0.1 <= |d| < 10^7, otherwise d.ddde<exp>. single: digits that round-trip
   as a float, for CFloat. */
static void show_digits(Buf *b, double d, bool single) {
  if (isnan(d)) { buf_puts(b, "NaN"); return; }
  if (isinf(d)) { buf_puts(b, d < 0 ? "-Infinity" : "Infinity"); return; }
  if (d == 0) { buf_puts(b, signbit(d) ? "-0.0" : "0.0"); return; }
  char tmp[64];
  for (int p = 1; p <= 17; p++) {
    snprintf(tmp, sizeof tmp, "%.*e", p - 1, d);
    if (single ? (float)strtod(tmp, NULL) == (float)d : strtod(tmp, NULL) == d) break;
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
  show_digits(b, d, false);
  if (paren) buf_putc(b, ')');
}

static void show(Buf *b, V v, int prec);

static void show_field(Buf *b, V v, int i, int prec) {
  if (raw_field(v, i)) show_float(b, fk_rawf(FK_FIELD(v, i)), prec);
  else show(b, FK_FIELD(v, i), prec);
}

/* i in decimal, ending at end; the first character. INT64_MIN negates as unsigned. */
static char *decimal(char *end, int64_t i) {
  uint64_t u = i < 0 ? 0 - (uint64_t)i : (uint64_t)i;
  do *--end = (char)('0' + u % 10); while (u /= 10);
  if (i < 0) *--end = '-';
  return end;
}

V fk_s_showint(int64_t i) {
  char tmp[24], *p = decimal(tmp + sizeof tmp, i);
  return fk_str(p, (size_t)(tmp + sizeof tmp - p));
}

static void show(Buf *b, V v, int prec) {
  FK_STACK_CHECK();
  char tmp[32];
  if (FK_ISINT(v)) {
    int64_t i = FK_INT(v);
    char *p = decimal(tmp + sizeof tmp - 1, i);
    tmp[sizeof tmp - 1] = 0;
    if (prec > 6 && i < 0) buf_putc(b, '(');
    buf_puts(b, p);
    if (prec > 6 && i < 0) buf_putc(b, ')');
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
  if (h->kind == K_STR) {
    buf_putc(b, '"');
    for (size_t i = 0; i < FK_STRLEN(v); i++) show_char_esc(b, (unsigned char)FK_STRBYTES(v)[i], '"');
    buf_putc(b, '"');
    return;
  }
  if (h->kind == K_PTR) { /* as Haskell shows a Ptr */
    snprintf(tmp, sizeof tmp, "0x%016llx", (unsigned long long)(uintptr_t)FK_PTRV(v));
    buf_puts(b, tmp);
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
      show_field(b, v, i, 0);
    }
    buf_putc(b, '}');
    if (prec > 10) buf_putc(b, ')');
    return;
  }
  if (prec > 10) buf_putc(b, '(');
  buf_puts(b, fk_cons[id].name);
  for (int i = 0; i < h->n; i++) {
    buf_putc(b, ' ');
    show_field(b, v, i, 11);
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

static int str_cmp(V a, V b) {
  size_t na = FK_STRLEN(a), nb = FK_STRLEN(b);
  int c = memcmp(FK_STRBYTES(a), FK_STRBYTES(b), na < nb ? na : nb);
  return c ? (c < 0 ? -1 : 1) : na < nb ? -1 : na > nb;
}

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
    if (FK_ISPTR(a) && FK_ISPTR(b) && FK_HDR(a)->kind == K_STR && FK_HDR(b)->kind == K_STR)
      return str_cmp(a, b);
    if (!FK_ISPTR(a) || !FK_ISPTR(b) || FK_HDR(a)->kind != K_CON || FK_HDR(b)->kind != K_CON)
      fk_error("cannot compare functions");
    uint32_t ia = FK_CONID(a), ib = FK_CONID(b);
    if (ia != ib) return ia < ib ? -1 : 1;
    int n = FK_HDR(a)->n;
    if (n == 0) return 0;
    for (int i = 0; i < n; i++)
      if (raw_field(a, i)) {
        double x = fk_rawf(FK_FIELD(a, i)), y = fk_rawf(FK_FIELD(b, i));
        if (x != y) return x < y ? -1 : 1;
      } else if (i < n - 1) {
        int c = fk_compare(FK_FIELD(a, i), FK_FIELD(b, i));
        if (c) return c;
      }
    if (raw_field(a, n - 1)) return 0;
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
   wraps at 64 bits (unsigned in C, so defined). Tagged Ints and Chars are
   ordered like their values, so they compare as words unless an Int is
   boxed. Float follows IEEE: NaN /= NaN. */
static inline int64_t fk_s_absi(int64_t x) { return x < 0 ? (int64_t)(0 - (uint64_t)x) : x; }

#define I2(name, expr)                                                         \
  static inline V fk_i_##name(V x, V y) { return expr; }                     \
  V fk_p_##name(V self, V *a) { return fk_i_##name(a[0], a[1]); }
#define I1(name, expr)                                                         \
  static inline V fk_i_##name(V x) { return expr; }                          \
  V fk_p_##name(V self, V *a) { return fk_i_##name(a[0]); }
#define IWRAP(op) FK_MKINT((int64_t)((uint64_t)FK_INT(x) op (uint64_t)FK_INT(y)))

I2(addi, IWRAP(+))
I2(subi, IWRAP(-))
I2(muli, IWRAP(*))
I2(addf, fk_float(FK_FLOATV(x) + FK_FLOATV(y)))
I2(subf, fk_float(FK_FLOATV(x) - FK_FLOATV(y)))
I2(mulf, fk_float(FK_FLOATV(x) * FK_FLOATV(y)))
I1(negi, FK_MKINT((int64_t)(0 - (uint64_t)FK_INT(x))))
I1(negf, fk_float(-FK_FLOATV(x)))
I1(absi, FK_MKINT(fk_s_absi(FK_INT(x))))
I1(absf, fk_float(fabs(FK_FLOATV(x))))
I2(fdiv, fk_float(FK_FLOATV(x) / FK_FLOATV(y)))

/* Comparisons also have a C truth value form, fk_b_NAME, for conditions. */
#define B2(name, expr)                                                         \
  static inline bool fk_b_##name(V x, V y) { return expr; }                  \
  I2(name, FK_BOOL(fk_b_##name(x, y)))

/* Int and Char: as words, unless one is a boxed Int. */
#define WORDS(x, y) (!FK_ISPTR(x) && !FK_ISPTR(y))
B2(eqw, x == y || (!WORDS(x, y) && FK_INT(x) == FK_INT(y)))
B2(new, !fk_b_eqw(x, y))
B2(ltw, WORDS(x, y) ? (intptr_t)x < (intptr_t)y : FK_INT(x) < FK_INT(y))
B2(lew, WORDS(x, y) ? (intptr_t)x <= (intptr_t)y : FK_INT(x) <= FK_INT(y))
B2(gtw, WORDS(x, y) ? (intptr_t)x > (intptr_t)y : FK_INT(x) > FK_INT(y))
B2(gew, WORDS(x, y) ? (intptr_t)x >= (intptr_t)y : FK_INT(x) >= FK_INT(y))
B2(eqf, FK_FLOATV(x) == FK_FLOATV(y))
B2(nef, FK_FLOATV(x) != FK_FLOATV(y))
B2(ltf, FK_FLOATV(x) < FK_FLOATV(y))
B2(lef, FK_FLOATV(x) <= FK_FLOATV(y))
B2(gtf, FK_FLOATV(x) > FK_FLOATV(y))
B2(gef, FK_FLOATV(x) >= FK_FLOATV(y))

_Noreturn static void div_zero(void) { fk_error("divide by zero"); }

/* Scalar forms, fk_s_NAME, for typed generated code. div and mod round
   toward negative infinity, quot and rem toward zero. minBound / -1
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
/* Bits, as Haskell's Data.Bits at Int: a shift by 64 or more gives 0, or -1
   for shiftR of a negative number; a negative shift is an error. */
static inline int64_t fk_s_shl(int64_t x, int64_t n) {
  if (n < 0) fk_error("shiftL: negative shift %lld", (long long)n);
  return n >= 64 ? 0 : (int64_t)((uint64_t)x << n);
}
static inline int64_t fk_s_shr(int64_t x, int64_t n) {
  if (n < 0) fk_error("shiftR: negative shift %lld", (long long)n);
  return n >= 64 ? (x < 0 ? -1 : 0) : x >> n; /* arithmetic, in the C compilers funk supports */
}
I2(band, FK_MKINT(FK_INT(x) & FK_INT(y)))
I2(bor, FK_MKINT(FK_INT(x) | FK_INT(y)))
I2(bxor, FK_MKINT(FK_INT(x) ^ FK_INT(y)))
I1(bnot, FK_MKINT(~FK_INT(x)))
I2(shl, FK_MKINT(fk_s_shl(FK_INT(x), FK_INT(y))))
I2(shr, FK_MKINT(fk_s_shr(FK_INT(x), FK_INT(y))))
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
  if (!(d >= -0x1p63 && d < 0x1p63)) fk_error("float out of Int range"); /* NaN fails too */
  return FK_MKINT((intptr_t)d);
}
V fk_p_floor(V self, V *a) { return to_int(floor(num_d(a[0]))); }
V fk_p_truncsat(V self, V *a) { return FK_MKINT(fk_s_truncsat(FK_FLOATV(a[0]))); }
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

V fk_p_showi(V self, V *a) { return fk_s_showint(FK_INT(a[0])); }

V fk_p_show(V self, V *a) {
  char *s = fk_show_cstr(a[0]);
  V r = fk_str(s, strlen(s));
  free(s);
  return r;
}

V fk_p_print(V self, V *a) {
  char *s = fk_show_cstr(a[0]);
  puts(s);
  free(s);
  return fk_unit;
}

/* The C types of foreign imports (ffi.md) are held as Int or Float words,
   wrapped to their width. */
V fk_p_idi(V self, V *a) { return a[0]; }
V fk_p_idf(V self, V *a) { return a[0]; }
V fk_p_tocint(V self, V *a) { return FK_MKINT((int32_t)(uint32_t)FK_INT(a[0])); }
V fk_p_tocuint(V self, V *a) { return FK_MKINT((uint32_t)FK_INT(a[0])); }
V fk_p_toclong(V self, V *a) { return FK_MKINT((long)FK_INT(a[0])); }
V fk_p_toculong(V self, V *a) { return FK_MKINT((int64_t)(unsigned long)FK_INT(a[0])); }
V fk_p_toccchar(V self, V *a) { return FK_MKINT((char)FK_INT(a[0])); }
V fk_p_tocbool(V self, V *a) { return FK_MKINT(FK_INT(a[0]) != 0); }
V fk_p_newcstring(V self, V *a) { /* freed with free */
  char *p = malloc(FK_STRLEN(a[0]) + 1);
  if (!p) fk_error("newCString: out of memory");
  return fk_ptr(memcpy(p, FK_STRBYTES(a[0]), FK_STRLEN(a[0]) + 1));
}
V fk_p_tocsize(V self, V *a) { return FK_MKINT((int64_t)(size_t)FK_INT(a[0])); }
V fk_p_tocfloat(V self, V *a) { return fk_float((float)FK_FLOATV(a[0])); }
V fk_p_ltu(V self, V *a) { return FK_BOOL((uint64_t)FK_INT(a[0]) < (uint64_t)FK_INT(a[1])); }

V fk_p_showu(V self, V *a) {
  char tmp[24];
  snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)(uint64_t)FK_INT(a[0]));
  return fk_str(tmp, strlen(tmp));
}

V fk_p_showcf(V self, V *a) {
  Buf b = {0};
  show_digits(&b, FK_FLOATV(a[0]), true);
  V r = fk_str(b.s, b.n);
  free(b.s);
  return r;
}

/* Ptr and C memory (ffi.md, phase 2). funk strings are NUL-terminated and
   never moved, so C reads one in place, during the call. */
const char *fk_cstr(V s) {
  const char *b = FK_STRBYTES(s);
  if (strlen(b) != FK_STRLEN(s)) fk_error("a String passed to C contains a NUL byte, which C would read as its end");
  return b;
}

V fk_cstr_result(const void *p, const char *fn) {
  if (!p) fk_error("`%s` returned NULL for a String; declare its result `Ptr CChar`, test it against nullPtr, and read it with peekCString", fn);
  return fk_str(p, strlen(p));
}

V fk_p_nullptr(V self, V *a) { return fk_ptr(NULL); }
V fk_p_eqptr(V self, V *a) { return FK_BOOL(FK_PTRV(a[0]) == FK_PTRV(a[1])); }
V fk_p_castptr(V self, V *a) { return a[0]; }
V fk_p_plusptr(V self, V *a) { return fk_ptr((void *)((uintptr_t)FK_PTRV(a[0]) + (uintptr_t)FK_INT(a[1]))); } /* as integers: plusPtr nullPtr n is defined */
V fk_p_ptrint(V self, V *a) { return FK_MKINT((int64_t)(uintptr_t)FK_PTRV(a[0])); }

V fk_p_mallocbytes(V self, V *a) {
  int64_t n = FK_INT(a[0]);
  void *p = n < 0 ? NULL : malloc(n ? (size_t)n : 1);
  if (!p) fk_error("mallocBytes %lld: out of memory", (long long)n);
  return fk_ptr(p);
}
V fk_p_free(V self, V *a) { free(FK_PTRV(a[0])); return fk_unit; }

/* peek and poke: the width in bytes, negative for an unsigned integer. */
V fk_p_peeki(V self, V *a) { return FK_MKINT(fk_s_peeki(FK_INT(a[0]), FK_PTRV(a[1]))); }
V fk_p_pokei(V self, V *a) { return fk_s_pokei(FK_INT(a[0]), FK_PTRV(a[1]), FK_INT(a[2])); }
V fk_p_peekf(V self, V *a) { return fk_float(fk_s_peekf(FK_INT(a[0]), FK_PTRV(a[1]))); }
V fk_p_pokef(V self, V *a) { return fk_s_pokef(FK_INT(a[0]), FK_PTRV(a[1]), FK_FLOATV(a[2])); }
/* Array a and Ring a; the scalar forms are in rt.h, for typed code. */
V fk_p_newarray(V self, V *a) {
  int64_t n = FK_INT(a[0]), size = FK_INT(a[1]);
  if (n < 1) fk_error("newArray: a length of %lld; an array has at least one element", (long long)n);
  if (size < 1 || n > (INT64_MAX - (int64_t)sizeof(FkArray)) / size) fk_error("newArray: %lld elements of %lld bytes is too large", (long long)n, (long long)size);
  FkArray *h = calloc(1, sizeof(FkArray) + (size_t)(n * size));
  if (!h) fk_error("newArray: out of memory");
  h->length = n;
  h->size = size;
  return fk_ptr(h);
}
V fk_p_freearray(V self, V *a) { free(FK_PTRV(a[0])); return fk_unit; }
V fk_p_alen(V self, V *a) { return FK_MKINT(fk_s_alen(FK_PTRV(a[0]))); }
V fk_p_asize(V self, V *a) { return FK_MKINT(fk_s_asize(FK_PTRV(a[0]))); }
V fk_p_aptr(V self, V *a) { return fk_ptr(fk_s_aptr(FK_PTRV(a[0]))); }
V fk_p_wrap(V self, V *a) { return FK_MKINT(fk_s_wrap(FK_INT(a[0]), FK_INT(a[1]))); }

V fk_p_newring(V self, V *a) {
  int64_t n = FK_INT(a[0]), size = FK_INT(a[1]), cap = 1;
  if (n < 1 || n > ((int64_t)1 << 40)) fk_error("newRing: a capacity of %lld", (long long)n);
  while (cap < n) cap *= 2;
  if (size < 1 || cap > (INT64_MAX - (int64_t)sizeof(FkRing)) / size) fk_error("newRing: %lld slots of %lld bytes is too large", (long long)cap, (long long)size);
  FkRing *g = calloc(1, sizeof(FkRing) + (size_t)(cap * size));
  if (!g) fk_error("newRing: out of memory");
  atomic_init(&g->head, 0);
  atomic_init(&g->tail, 0);
  g->cap = cap;
  g->size = size;
  return fk_ptr(g);
}
V fk_p_freering(V self, V *a) { free(FK_PTRV(a[0])); return fk_unit; }
V fk_p_rcount(V self, V *a) { return FK_MKINT(fk_s_rcount(FK_PTRV(a[0]))); }
V fk_p_rspace(V self, V *a) { return FK_MKINT(fk_s_rspace(FK_PTRV(a[0]))); }
V fk_p_rback(V self, V *a) { return fk_ptr(fk_s_rback(FK_PTRV(a[0]))); }
V fk_p_rcommit(V self, V *a) { return fk_s_rcommit(FK_PTRV(a[0])); }
V fk_p_rfront(V self, V *a) { return fk_ptr(fk_s_rfront(FK_PTRV(a[0]))); }
V fk_p_rdrop(V self, V *a) { return fk_s_rdrop(FK_PTRV(a[0])); }

V fk_p_apeeki(V self, V *a) { return FK_MKINT(fk_s_apeeki(FK_INT(a[0]), FK_PTRV(a[1]))); }
V fk_p_apokei(V self, V *a) { return fk_s_apokei(FK_INT(a[0]), FK_PTRV(a[1]), FK_INT(a[2])); }
V fk_p_apeekf(V self, V *a) { return fk_float(fk_s_apeekf(FK_INT(a[0]), FK_PTRV(a[1]))); }
V fk_p_apokef(V self, V *a) { return fk_s_apokef(FK_INT(a[0]), FK_PTRV(a[1]), FK_FLOATV(a[2])); }
V fk_p_aaddi(V self, V *a) { return FK_MKINT(fk_s_aaddi(FK_INT(a[0]), FK_PTRV(a[1]), FK_INT(a[2]))); }
V fk_p_acasi(V self, V *a) { return FK_BOOL(fk_s_acasi(FK_INT(a[0]), FK_PTRV(a[1]), FK_INT(a[2]), FK_INT(a[3]))); }
V fk_p_aaddf(V self, V *a) { return fk_float(fk_s_aaddf(FK_INT(a[0]), FK_PTRV(a[1]), FK_FLOATV(a[2]))); }
V fk_p_acasf(V self, V *a) { return FK_BOOL(fk_s_acasf(FK_INT(a[0]), FK_PTRV(a[1]), FK_FLOATV(a[2]), FK_FLOATV(a[3]))); }
V fk_p_peekp(V self, V *a) { return fk_ptr(*(void **)FK_PTRV(a[0])); }
V fk_p_pokep(V self, V *a) { *(void **)FK_PTRV(a[0]) = FK_PTRV(a[1]); return fk_unit; }
V fk_p_peekcstring(V self, V *a) { return fk_cstr_result(FK_PTRV(a[0]), "peekCString"); }

/* Callbacks (ffi.md, phase 3). A StablePtr is an index, plus one, into a
   root table, so a value C holds stays alive and does not move. */
static V *stables;
static bool *stable_live;
static size_t nstables, capstables, free_stable; /* a freed index plus one; a freed slot holds the next */

V fk_p_newstable(V self, V *a) {
  size_t i;
  if (free_stable) {
    i = free_stable - 1;
    free_stable = (size_t)FK_INT(stables[i]);
  } else {
    if (nstables == capstables) {
      capstables = capstables ? 2 * capstables : 64;
      stables = rt_realloc(stables, capstables * sizeof *stables);
      stable_live = rt_realloc(stable_live, capstables * sizeof *stable_live);
      if (nstables == 0) fk_add_roots(&stables, &nstables);
    }
    i = nstables++;
  }
  stables[i] = a[0];
  stable_live[i] = true;
  return fk_ptr((void *)(uintptr_t)(i + 1));
}

static size_t stable_index(void *p) {
  size_t i = (size_t)(uintptr_t)p;
  if (i == 0 || i > nstables || !stable_live[i - 1]) fk_error("%p is not a live StablePtr; was it freed?", p);
  return i - 1;
}

V fk_stable_deref(void *p) { return stables[stable_index(p)]; }
V fk_p_derefstable(V self, V *a) { return fk_stable_deref(FK_PTRV(a[0])); }
V fk_p_freestable(V self, V *a) {
  size_t i = stable_index(FK_PTRV(a[0]));
  stables[i] = FK_MKINT((int64_t)free_stable);
  stable_live[i] = false;
  free_stable = i + 1;
  return fk_unit;
}

static pthread_t funk_thread; /* set by task_main */

bool fk_on_funk_thread(void) { return pthread_equal(pthread_self(), funk_thread); }

_Noreturn void fk_callback_thread(const char *name) {
  fprintf(stderr, "funk: callback `%s` was called on a thread other than funk's; declare it \"queue\" instead of \"callback\"\n", name);
  abort();
}

/* A runtime error in a direct callback stops at its trampoline, which
   returns to C; the foreign call that led to it raises it again. */
int fk_cb_failed;
_Noreturn void fk_cb_rethrow(void) {
  fk_cb_failed = 0;
  if (fk_error_jmp) longjmp(*fk_error_jmp, 1);
  exit(1);
}

/* Queued callbacks (realtime.md, R3): a trampoline on any thread claims a
   slot in a fixed ring, writes its arguments into it and publishes it; no
   malloc, no lock. The ring is Vyukov's bounded queue: a slot's sequence
   number says whose turn it is. A full ring drops the event and counts it.
   waitEvent runs the next event's closure on funk's thread. */
#define RING_SLOTS 4096
static FkEvent ring[RING_SLOTS];
static _Atomic size_t ring_head; /* the next slot to claim */
static size_t ring_tail;         /* the next slot to run: funk's thread only */
static _Atomic size_t ring_dropped;
#ifdef __APPLE__
static dispatch_semaphore_t ring_sem; /* one count per published event */
#define SEM_POST() dispatch_semaphore_signal(ring_sem)
#define SEM_WAIT() dispatch_semaphore_wait(ring_sem, DISPATCH_TIME_FOREVER)
#define SEM_TRY() (dispatch_semaphore_wait(ring_sem, DISPATCH_TIME_NOW) == 0)
#else
static sem_t ring_sem;
#define SEM_POST() sem_post(&ring_sem)
#define SEM_WAIT() while (sem_wait(&ring_sem) != 0)
#define SEM_TRY() (sem_trywait(&ring_sem) == 0)
#endif

static void ring_init(void) {
  for (size_t i = 0; i < RING_SLOTS; i++) atomic_init(&ring[i].seq, i);
#ifdef __APPLE__
  ring_sem = dispatch_semaphore_create(0);
#else
  sem_init(&ring_sem, 0, 0);
#endif
}

FkEvent *fk_event_claim(void) {
  size_t pos = atomic_load_explicit(&ring_head, memory_order_relaxed);
  for (;;) {
    FkEvent *e = &ring[pos % RING_SLOTS];
    size_t seq = atomic_load_explicit(&e->seq, memory_order_acquire);
    intptr_t dif = (intptr_t)seq - (intptr_t)pos;
    if (dif == 0) {
      if (atomic_compare_exchange_weak_explicit(&ring_head, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) return e;
    } else if (dif < 0) { /* the slot is a lap behind: the ring is full */
      atomic_fetch_add_explicit(&ring_dropped, 1, memory_order_relaxed);
      return NULL;
    } else {
      pos = atomic_load_explicit(&ring_head, memory_order_relaxed);
    }
  }
}

void fk_event_publish(FkEvent *e) {
  atomic_store_explicit(&e->seq, atomic_load_explicit(&e->seq, memory_order_relaxed) + 1, memory_order_release);
  SEM_POST();
}

void fk_event_release(FkEvent *e) { /* the slot is free once its arguments are read */
  atomic_store_explicit(&e->seq, ring_tail + RING_SLOTS, memory_order_release);
  ring_tail++;
}

void *fk_event_copy(FkEvent *e, size_t *used, const void *p, size_t n) {
  if (!p) return NULL;
  void *d = *used + n <= FK_EVENT_DATA ? e->data + *used : malloc(n ? n : 1); /* malloc only if the bytes do not fit */
  if (!d) return NULL;
  if (d == e->data + *used) *used += n;
  if (n) memcpy(d, p, n);
  return d;
}

void fk_event_uncopy(FkEvent *e, void *p) {
  if (p && ((unsigned char *)p < e->data || (unsigned char *)p >= e->data + FK_EVENT_DATA)) free(p);
}

/* The next event, once its producer has published it: a producer that
   claimed an earlier slot may still be writing, for a few instructions. */
static void run_next(void) {
  FkEvent *e = &ring[ring_tail % RING_SLOTS];
  while (atomic_load_explicit(&e->seq, memory_order_acquire) != ring_tail + 1) sched_yield();
  if (e->run) e->run(e); /* run releases the slot before calling funk */
  else fk_event_release(e); /* its bytes could not be copied: dropped */
}

V fk_p_waitevent(V self, V *a) {
  SEM_WAIT();
  run_next();
  return fk_unit;
}

V fk_p_pollevent(V self, V *a) {
  if (!SEM_TRY()) return fk_false;
  run_next();
  return fk_true;
}

V fk_p_droppedevents(V self, V *a) { return FK_MKINT((int64_t)atomic_load(&ring_dropped)); }

/* show for a String, and for a [Char], which shows as one: quoted, escaped. */
V fk_p_showstr(V self, V *a) { return fk_p_show(self, a); }

V fk_p_showchars(V self, V *a) {
  Buf b = {0};
  buf_putc(&b, '"');
  for (V l = a[0]; FK_CONID(l) == FK_C_CONS; l = FK_FIELD(l, 1)) show_char_esc(&b, FK_CHAR(FK_FIELD(l, 0)), '"');
  buf_putc(&b, '"');
  V r = fk_str(b.s, b.n);
  free(b.s);
  return r;
}

/* ---------------------------------------------------------- String prims */

V fk_p_streq(V self, V *a) { return FK_BOOL(str_cmp(a[0], a[1]) == 0); }
V fk_p_strne(V self, V *a) { return FK_BOOL(str_cmp(a[0], a[1]) != 0); }
V fk_p_strlt(V self, V *a) { return FK_BOOL(str_cmp(a[0], a[1]) < 0); }
V fk_p_strle(V self, V *a) { return FK_BOOL(str_cmp(a[0], a[1]) <= 0); }
V fk_p_strgt(V self, V *a) { return FK_BOOL(str_cmp(a[0], a[1]) > 0); }
V fk_p_strge(V self, V *a) { return FK_BOOL(str_cmp(a[0], a[1]) >= 0); }
V fk_p_strlength(V self, V *a) { return FK_MKINT((intptr_t)FK_STRLEN(a[0])); }

V fk_p_strappend(V self, V *a) {
  size_t n = FK_STRLEN(a[0]), m = FK_STRLEN(a[1]);
  if (!n) return a[1];
  if (!m) return a[0];
  if (n + m > UINT32_MAX) fk_error("string too long");
  V o = alloc(str_words(n + m), K_STR, 0, (uint32_t)(n + m));
  memcpy(FK_STRBYTES(o), FK_STRBYTES(a[0]), n);
  memcpy(FK_STRBYTES(o) + n, FK_STRBYTES(a[1]), m + 1);
  return o;
}

V fk_p_pack(V self, V *a) {
  size_t n = 0;
  for (V l = a[0]; FK_CONID(l) == FK_C_CONS; l = FK_FIELD(l, 1)) n++;
  if (n > UINT32_MAX) fk_error("string too long");
  V o = alloc(str_words(n), K_STR, 0, (uint32_t)n);
  char *p = FK_STRBYTES(o);
  for (V l = a[0]; FK_CONID(l) == FK_C_CONS; l = FK_FIELD(l, 1)) *p++ = (char)FK_CHAR(FK_FIELD(l, 0));
  *p = 0;
  return o;
}

V fk_p_unpack(V self, V *a) {
  V s = a[0], acc = fk_nil;
  for (size_t n = FK_STRLEN(s); n-- > 0;) acc = fk_mkcon(FK_C_CONS, 2, (V[]){FK_MKCHAR(FK_STRBYTES(s)[n]), acc});
  return acc;
}

/* Split s into pieces at sep (lines) or at runs of spaces (words). The
   result list is built front to back; the string is re-read after each
   allocation, which a collection never moves. */
static bool is_space(int c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

static V split(V s, bool words) {
  V head = fk_nil, *dst = &head;
  size_t n = FK_STRLEN(s), i = 0;
  while (i < n) {
    if (words) {
      while (i < n && is_space((unsigned char)FK_STRBYTES(s)[i])) i++;
      if (i == n) break;
    }
    size_t j = i;
    while (j < n && (words ? !is_space((unsigned char)FK_STRBYTES(s)[j]) : FK_STRBYTES(s)[j] != '\n')) j++;
    V piece = alloc(str_words(j - i), K_STR, 0, (uint32_t)(j - i));
    memcpy(FK_STRBYTES(piece), FK_STRBYTES(s) + i, j - i);
    FK_STRBYTES(piece)[j - i] = 0;
    V cell = fk_mkcon(FK_C_CONS, 2, (V[]){piece, fk_nil});
    *dst = cell;
    dst = &FK_FIELD(cell, 1);
    i = words ? j : j + 1;
  }
  return head;
}

V fk_p_words(V self, V *a) { return split(a[0], true); }
V fk_p_lines(V self, V *a) { return split(a[0], false); }

/* Join a list of Strings: unwords puts a space between, unlines a newline
   after each, concatStrings nothing. One allocation, linear time. */
enum { J_CONCAT, J_WORDS, J_LINES };

static V join(V l, int mode) {
  size_t n = 0, k = 0;
  for (V p = l; FK_CONID(p) == FK_C_CONS; p = FK_FIELD(p, 1), k++) n += FK_STRLEN(FK_FIELD(p, 0));
  n += mode == J_LINES ? k : mode == J_WORDS && k ? k - 1 : 0;
  if (n > UINT32_MAX) fk_error("string too long");
  V o = alloc(str_words(n), K_STR, 0, (uint32_t)n);
  char *q = FK_STRBYTES(o);
  bool first = true;
  for (V p = l; FK_CONID(p) == FK_C_CONS; p = FK_FIELD(p, 1), first = false) {
    V s = FK_FIELD(p, 0);
    if (mode == J_WORDS && !first) *q++ = ' ';
    memcpy(q, FK_STRBYTES(s), FK_STRLEN(s));
    q += FK_STRLEN(s);
    if (mode == J_LINES) *q++ = '\n';
  }
  *q = 0;
  return o;
}

V fk_p_unwords(V self, V *a) { return join(a[0], J_WORDS); }
V fk_p_unlines(V self, V *a) { return join(a[0], J_LINES); }
V fk_p_concatstr(V self, V *a) { return join(a[0], J_CONCAT); }

/* Constructor index, for derived Ord. */
V fk_p_contag(V self, V *a) {
  if (!FK_ISPTR(a[0]) || FK_HDR(a[0])->kind != K_CON) fk_error("primConTag: not a constructor");
  return FK_MKINT(FK_CONID(a[0]));
}

V fk_p_putstr(V self, V *a) {
  fwrite(FK_STRBYTES(a[0]), 1, FK_STRLEN(a[0]), stdout);
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
  V r = fk_str(b.s, b.n);
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

/* trace msg x prints msg to standard error and returns x: a debugging aid
   that is pure in its type (typed-effects.md). Standard output is flushed
   first, so the two streams stay in program order. */
V fk_p_trace(V self, V *a) {
  fflush(stdout);
  fwrite(FK_STRBYTES(a[0]), 1, FK_STRLEN(a[0]), stderr);
  fputc('\n', stderr);
  return a[1];
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
  ring_init();
  const char *s = getenv("FUNK_GCSTATS");
  if ((gcstats = s && *s && strcmp(s, "0") != 0)) atexit(gc_summary);
  s = getenv("FUNK_GCSTRESS");
  if ((gcstress = s && *s && strcmp(s, "0") != 0)) {
    gc_min = gc_threshold = (size_t)64 << 10;
    step_bytes = (size_t)16 << 10;
  }
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


static void on_fault(int sig, siginfo_t *si, void *ctx) {
  char *a = si->si_addr;
  if (a >= fk_snap_lo && a < snap_hi) { /* a write to the stack's snapshot: keep the page, then let the write happen */
    char *pg = (char *)((uintptr_t)a & ~(uintptr_t)(page_size - 1));
    size_t i = (size_t)(pg - fk_snap_lo) / page_size;
    if (!snap_copied[i]) {
      memcpy(snap_area + i * page_size, pg, page_size);
      snap_copied[i] = 1;
    }
    mprotect(pg, page_size, PROT_READ | PROT_WRITE);
    return;
  }
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
  stack_top = __builtin_frame_address(0);
  funk_thread = pthread_self();
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
