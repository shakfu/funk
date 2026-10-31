#define _DEFAULT_SOURCE
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/wait.h>

#include "funk.h"

extern char **environ;

/* The compiler's memory: each block is on its session's list, so the
   session's end frees what is left (compiler_free_all). A header per block,
   16 bytes, keeps malloc's alignment; an arena would need one too, for
   xrealloc's old size, and could not free. */
typedef struct Block {
  struct Block *prev, *next;
} Block;
_Static_assert(sizeof(Block) == 16, "a block's data stays 16-byte aligned");
#define BLOCKS (*(Block **)&fk_here()->cc_blocks)
#define REGION (*(Block **)&fk_here()->cc_region)

static void *track_on(Block **head, Block *b) {
  if (!b) fk_fatal("out of memory");
  b->prev = NULL;
  b->next = *head;
  if (b->next) b->next->prev = b;
  *head = b;
  return b + 1;
}

/* Only xmalloc and xcalloc use the region: a growing vector stays permanent,
   so state that keeps its capacity across chunks never points into a region. */
static void *track(Block *b) { return track_on(fk_here()->cc_region_on ? &REGION : &BLOCKS, b); }

static void untrack(Block *b) {
  if (b->prev) b->prev->next = b->next;
  else if (BLOCKS == b) BLOCKS = b->next;
  else REGION = b->next;
  if (b->next) b->next->prev = b->prev;
}

void *compile_region_begin(void) {
  void *outer = REGION;
  REGION = NULL;
  fk_here()->cc_region_on = true;
  return outer;
}

void compile_region(bool on) { fk_here()->cc_region_on = on; }

static void free_list(Block *b) {
  for (Block *next; b; b = next) {
    next = b->next;
    free(b);
  }
}

void compile_region_end(void *outer, bool keep) {
  fk_here()->cc_region_on = false;
  while (keep && REGION) {
    Block *b = REGION;
    untrack(b);
    track_on(&BLOCKS, b);
  }
  free_list(REGION);
  REGION = outer;
}

/* A runaway pass reaches 4 GiB/s, too fast for a timeout. Going over is a
   compile error, so a REPL or plugin session survives; what was allocated
   stays until the session ends. Counts bytes allocated, not live. */
void compile_budget(bool on) {
  const char *s = on ? getenv("FK_COMPILE_LIMIT") : NULL;
  fk_here()->cc_budget = !on ? 0 : s ? strtoull(s, NULL, 10) : (size_t)2 << 30;
}

static void charge(size_t n) {
  size_t *left = &fk_here()->cc_budget;
  if (!*left) return;
  if (n >= *left) {
    *left = 0;
    cerror(0, 0, "the compiler exceeded its memory limit (FK_COMPILE_LIMIT, default 2 GiB)");
  }
  *left -= n;
}

void *xmalloc(size_t n) {
  charge(n);
  return track(malloc(sizeof(Block) + n));
}

void *xcalloc(size_t n, size_t size) {
  if (size && n > (SIZE_MAX - sizeof(Block)) / size) fk_fatal("out of memory");
  charge(n * size);
  return track(calloc(1, sizeof(Block) + n * size));
}

void *xrealloc(void *p, size_t n) {
  charge(n);
  if (!p) return track_on(&BLOCKS, malloc(sizeof(Block) + n));
  Block *b = (Block *)p - 1; /* keeps its place on its list */
  bool heads_blocks = BLOCKS == b, heads_region = REGION == b;
  Block *nb = realloc(b, sizeof(Block) + n);
  if (!nb) fk_fatal("out of memory");
  if (nb->prev) nb->prev->next = nb;
  if (nb->next) nb->next->prev = nb;
  if (heads_blocks) BLOCKS = nb;
  if (heads_region) REGION = nb;
  return nb + 1;
}

void xfree(void *p) {
  if (!p) return;
  Block *b = (Block *)p - 1;
  untrack(b);
  free(b);
}

void compiler_free_all(void) {
  free_list(BLOCKS);
  free_list(REGION);
  BLOCKS = REGION = NULL;
}

static char *vxsprintf(const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  char *s = xmalloc((size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, fmt, ap);
  return s;
}

char *xsprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = vxsprintf(fmt, ap);
  va_end(ap);
  return s;
}

char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  SB b = {0};
  char buf[4096];
  size_t n;
  sb_puts(&b, "");
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    if (b.n + n + 1 > b.cap) {
      b.cap = (b.n + n + 1) * 2;
      b.s = xrealloc(b.s, b.cap);
    }
    memcpy(b.s + b.n, buf, n);
    b.n += n;
    b.s[b.n] = 0;
  }
  fclose(f);
  *len = b.n;
  return b.s;
}

/* ------------------------------------------------------------- interning */

typedef struct { Map *m; const char *key; int old; } Undo;

/* util.c's state, one per session (embedding.md, phase 2) */
typedef struct {
  const char **itab;
  size_t icap, icount;
  VEC(Undo) undo_log;
} UtilState;
static const UtilState util_init = {0};
#define ST ((UtilState *)fk_here()->cc[CC_UTIL])

void *util_state_new(void) {
  UtilState *s = malloc(sizeof *s);
  if (!s) fk_fatal("out of memory");
  *s = util_init;
  return s;
}


static uint64_t hash(const char *s, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
  return h;
}

const char *intern_n(const char *s, size_t n) {
  bool region = fk_here()->cc_region_on; /* names and the table are permanent */
  fk_here()->cc_region_on = false;
  if (ST->icount * 2 >= ST->icap) {
    size_t ncap = ST->icap ? ST->icap * 2 : 1024;
    const char **nt = xcalloc(ncap, sizeof *nt);
    for (size_t i = 0; i < ST->icap; i++)
      if (ST->itab[i]) {
        size_t j = hash(ST->itab[i], strlen(ST->itab[i])) & (ncap - 1);
        while (nt[j]) j = (j + 1) & (ncap - 1);
        nt[j] = ST->itab[i];
      }
    xfree(ST->itab);
    ST->itab = nt;
    ST->icap = ncap;
  }
  size_t j = hash(s, n) & (ST->icap - 1);
  while (ST->itab[j]) {
    if (strlen(ST->itab[j]) == n && memcmp(ST->itab[j], s, n) == 0) {
      fk_here()->cc_region_on = region;
      return ST->itab[j];
    }
    j = (j + 1) & (ST->icap - 1);
  }
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  ST->itab[j] = p;
  ST->icount++;
  fk_here()->cc_region_on = region;
  return p;
}

const char *intern(const char *s) { return intern_n(s, strlen(s)); }

/* ----------------------------------------------------------- string buf */

void sb_puts(SB *b, const char *s) {
  size_t n = strlen(s);
  if (b->n + n + 1 > b->cap) {
    size_t cap = (b->n + n + 1) * 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
  }
  memcpy(b->s + b->n, s, n + 1);
  b->n += n;
}

void sb_printf(SB *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = vxsprintf(fmt, ap);
  va_end(ap);
  sb_puts(b, s);
  xfree(s);
}

/* ------------------------------------------------------------------- map */

static int map_slot(Map *m, const char *key) {
  size_t j = ((uintptr_t)key >> 3) & (size_t)(m->cap - 1);
  while (m->keys[j] && m->keys[j] != key) j = (j + 1) & (size_t)(m->cap - 1);
  return (int)j;
}

int map_get(Map *m, const char *key) {
  if (!m->cap) return -1;
  int j = map_slot(m, key);
  return m->keys[j] ? m->vals[j] : -1;
}

static void map_set(Map *m, const char *key, int val) {
  if (m->n * 2 >= m->cap) {
    bool region = fk_here()->cc_region_on; /* a map is permanent */
    fk_here()->cc_region_on = false;
    Map nm = {xcalloc((size_t)(m->cap ? 2 * m->cap : 256), sizeof(char *)), NULL,
              m->cap ? 2 * m->cap : 256, 0};
    nm.vals = xcalloc((size_t)nm.cap, sizeof(int));
    for (int i = 0; i < m->cap; i++)
      if (m->keys[i]) {
        int j = map_slot(&nm, m->keys[i]);
        nm.keys[j] = m->keys[i];
        nm.vals[j] = m->vals[i];
        nm.n++;
      }
    xfree(m->keys);
    xfree(m->vals);
    *m = nm;
    fk_here()->cc_region_on = region;
  }
  int j = map_slot(m, key);
  if (!m->keys[j]) { m->keys[j] = key; m->n++; }
  m->vals[j] = val;
}

void map_put(Map *m, const char *key, int val) {
  VPUSH(ST->undo_log, ((Undo){m, key, map_get(m, key)}));
  map_set(m, key, val);
}

int undo_mark(void) { return ST->undo_log.n; }

void undo_to(int mark) {
  while (ST->undo_log.n > mark) {
    Undo u = ST->undo_log.a[--ST->undo_log.n];
    map_set(u.m, u.key, u.old); /* -1 reads back as absent */
  }
}

/* ---------------------------------------------------------------- errors */


_Noreturn void cerror(int line, int col, const char *fmt, ...) {
  va_list ap;
  if (CS->cerr_quiet && CS->cerr_jmp) {
    fk_unwinding();
    longjmp(*CS->cerr_jmp, 1);
  }
  char *prefix = xsprintf("%s:%d:%d: error: ", CS->cur_file, line, col);
  va_start(ap, fmt);
  fk_vmessage(prefix, fmt, ap);
  va_end(ap);
  xfree(prefix);
  if (CS->cerr_jmp) {
    fk_unwinding();
    longjmp(*CS->cerr_jmp, 1);
  }
  exit(1);
}

void cwarning(int line, int col, const char *fmt, ...) {
  va_list ap;
  char *prefix = xsprintf("%s:%d:%d: warning: ", CS->cur_file, line, col);
  va_start(ap, fmt);
  fk_vmessage(prefix, fmt, ap);
  va_end(ap);
  xfree(prefix);
}

Argv cc_argv(void) {
  const char *cc = getenv("CC");
  Argv a = {0};
  for (char *w = strtok(xsprintf("%s", cc && *cc ? cc : "cc"), " "); w; w = strtok(NULL, " ")) VPUSH(a, w);
  return a;
}

int spawn_wait(Argv *a) {
  VPUSH(*a, NULL);
  pid_t pid;
  if (posix_spawnp(&pid, a->a[0], NULL, NULL, a->a, environ)) return -1;
  int status;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
