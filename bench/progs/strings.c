/* Strings as linked lists of characters, as in funk. */
#include "arena.h"

static Cell *show(long i) {
  char buf[32];
  int len = snprintf(buf, sizeof buf, "%ld", i);
  Cell *s = NULL;
  while (len-- > 0) s = cons(buf[len], s);
  return s;
}

/* xs ++ ys copies xs. */
static Cell *append(Cell *xs, Cell *ys) {
  Cell *out = ys, **tp = &out;
  for (; xs; xs = xs->tail) { *tp = cons(xs->head, ys); tp = &(*tp)->tail; }
  return out;
}

int main(void) {
  long n = read_n();
  Cell *s = NULL;
  for (long i = n; i >= 1; i--) s = append(show(i), i == n ? s : cons(' ', s));
  long len = 0, words = 0;
  for (Cell *c = s; c; c = c->tail) len++;
  for (Cell *c = s; c;) { /* each word is copied, as takeWhile does */
    while (c && c->head == ' ') c = c->tail;
    if (!c) break;
    Cell *w = NULL, **tp = &w;
    for (; c && c->head != ' '; c = c->tail) { *tp = cons(c->head, NULL); tp = &(*tp)->tail; }
    words++;
  }
  printf("%ld\n", len + words);
}
