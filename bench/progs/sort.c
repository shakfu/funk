/* The prelude's sortBy: singleton lists merged pairwise until one remains. */
#include "arena.h"

typedef struct Lists { Cell *list; struct Lists *next; } Lists;

static Lists *lcons(Cell *l, Lists *next) {
  Lists *c = alloc(sizeof *c);
  c->list = l;
  c->next = next;
  return c;
}

/* Stable: takes from ys only when x > y, like `cmp x y == GT`. */
static Cell *merge(Cell *xs, Cell *ys) {
  Cell *out = NULL, **tp = &out;
  while (xs && ys) {
    if (xs->head > ys->head) { *tp = cons(ys->head, NULL); ys = ys->tail; }
    else { *tp = cons(xs->head, NULL); xs = xs->tail; }
    tp = &(*tp)->tail;
  }
  for (Cell *r = xs ? xs : ys; r; r = r->tail) { *tp = cons(r->head, NULL); tp = &(*tp)->tail; }
  return out;
}

int main(void) {
  long n = read_n(), s = 42;
  Lists *xss = NULL, **tp = &xss;
  for (long i = 0; i < n; i++) {
    *tp = lcons(cons(s, NULL), NULL);
    tp = &(*tp)->next;
    s = (s * 1103515245 + 12345) % 2147483648;
  }
  while (xss && xss->next) {
    Lists *out = NULL, **op = &out;
    for (Lists *p = xss; p; p = p->next ? p->next->next : NULL) {
      *op = lcons(p->next ? merge(p->list, p->next->list) : p->list, NULL);
      op = &(*op)->next;
    }
    xss = out;
  }
  long acc = 0;
  for (Cell *c = xss ? xss->list : NULL; c; c = c->tail) acc = (acc * 31 + c->head) % 1000000007;
  printf("%ld\n", acc);
}
