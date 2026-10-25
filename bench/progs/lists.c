#include "arena.h"

/* Each list is built in order, as funk's map and filter do; xs is kept for length. */
static long round_(long n) {
  Cell *xs = NULL, **tp = &xs;
  for (long i = 1; i <= n; i++) { *tp = cons(i, NULL); tp = &(*tp)->tail; }
  Cell *ev = NULL, **ep = &ev;
  for (Cell *c = xs; c; c = c->tail)
    if (c->head % 2 == 0) { *ep = cons(c->head, NULL); ep = &(*ep)->tail; }
  Cell *ms = NULL, **mp = &ms;
  for (Cell *c = ev; c; c = c->tail) { *mp = cons(c->head * 3, NULL); mp = &(*mp)->tail; }
  long s = 0;
  for (Cell *c = ms; c; c = c->tail) s += c->head;
  long len = 0;
  for (Cell *c = xs; c; c = c->tail) len++;
  return s + len;
}

int main(void) {
  long n = read_n(), acc = 0;
  for (int k = 0; k < 20; k++) {
    char *m = arena_mark();
    acc = (acc + round_(n)) % 1000000007;
    arena_reset(m);
  }
  printf("%ld\n", acc);
}
