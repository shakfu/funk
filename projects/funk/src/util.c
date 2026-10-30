#define _DEFAULT_SOURCE
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/wait.h>

#include "funk.h"

extern char **environ;

void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) fk_fatal("out of memory");
  return p;
}

void *xcalloc(size_t n, size_t size) {
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (!p) fk_fatal("out of memory");
  return p;
}

void *xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) fk_fatal("out of memory");
  return p;
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
  if (ST->icount * 2 >= ST->icap) {
    size_t ncap = ST->icap ? ST->icap * 2 : 1024;
    const char **nt = xcalloc(ncap, sizeof *nt);
    for (size_t i = 0; i < ST->icap; i++)
      if (ST->itab[i]) {
        size_t j = hash(ST->itab[i], strlen(ST->itab[i])) & (ncap - 1);
        while (nt[j]) j = (j + 1) & (ncap - 1);
        nt[j] = ST->itab[i];
      }
    free(ST->itab);
    ST->itab = nt;
    ST->icap = ncap;
  }
  size_t j = hash(s, n) & (ST->icap - 1);
  while (ST->itab[j]) {
    if (strlen(ST->itab[j]) == n && memcmp(ST->itab[j], s, n) == 0) return ST->itab[j];
    j = (j + 1) & (ST->icap - 1);
  }
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  ST->itab[j] = p;
  ST->icount++;
  return p;
}

const char *intern(const char *s) { return intern_n(s, strlen(s)); }

/* ----------------------------------------------------------- string buf */

void sb_puts(SB *b, const char *s) {
  size_t n = strlen(s);
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->s = xrealloc(b->s, b->cap);
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
  free(s);
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
    free(m->keys);
    free(m->vals);
    *m = nm;
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
  if (CS->cerr_quiet && CS->cerr_jmp) longjmp(*CS->cerr_jmp, 1);
  char *prefix = xsprintf("%s:%d:%d: error: ", CS->cur_file, line, col);
  va_start(ap, fmt);
  fk_vmessage(prefix, fmt, ap);
  va_end(ap);
  free(prefix);
  if (CS->cerr_jmp) longjmp(*CS->cerr_jmp, 1);
  exit(1);
}

void cwarning(int line, int col, const char *fmt, ...) {
  va_list ap;
  char *prefix = xsprintf("%s:%d:%d: warning: ", CS->cur_file, line, col);
  va_start(ap, fmt);
  fk_vmessage(prefix, fmt, ap);
  va_end(ap);
  free(prefix);
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
