/* arena.h -- bump allocator for the C benchmarks. Lists and trees are
   allocated like funk's heap objects; arena_reset frees a whole phase,
   standing in for the GC. Pages are committed only as they are touched. */
#include <stdio.h>
#include <stdlib.h>

#define ARENA_SIZE ((size_t)8 << 30)

static char *arena_base, *arena_ptr;

static inline void arena_init(void) {
  if (arena_base) return;
  arena_base = arena_ptr = malloc(ARENA_SIZE);
  if (!arena_base) { fputs("arena: out of memory\n", stderr); exit(1); }
}

static inline void *alloc(size_t n) {
  arena_init();
  n = (n + 15) & ~(size_t)15;
  if ((size_t)(arena_ptr - arena_base) + n > ARENA_SIZE) { fputs("arena: exhausted\n", stderr); exit(1); }
  void *p = arena_ptr;
  arena_ptr += n;
  return p;
}

static inline char *arena_mark(void) {
  arena_init();
  return arena_ptr;
}

static inline void arena_reset(char *mark) { arena_ptr = mark; }

static inline long read_n(void) {
  long n;
  if (scanf("%ld", &n) != 1) { fputs("expected an integer on stdin\n", stderr); exit(1); }
  return n;
}

typedef struct Cell { long head; struct Cell *tail; } Cell;

static inline Cell *cons(long h, Cell *t) {
  Cell *c = alloc(sizeof *c);
  c->head = h;
  c->tail = t;
  return c;
}
