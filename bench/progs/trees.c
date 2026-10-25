#include "arena.h"

typedef struct Tree { struct Tree *l, *r; } Tree;

static Tree *make(long d) {
  Tree *t = alloc(sizeof *t);
  t->l = d ? make(d - 1) : NULL; /* NULL is Leaf */
  t->r = d ? make(d - 1) : NULL;
  return t;
}

static long check(Tree *t) { return t ? 1 + check(t->l) + check(t->r) : 0; }

int main(void) {
  long maxd = read_n();
  Tree *lng = make(maxd);
  long acc = check(lng);
  for (long d = 4; d <= maxd; d += 2) {
    for (long i = 1L << (maxd - d + 4); i > 0; i--) {
      char *m = arena_mark();
      acc += check(make(d));
      arena_reset(m);
    }
  }
  printf("%ld\n", acc + check(lng));
}
