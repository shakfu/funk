/* Every partial board of size k as a list, extended by one queen per round;
   boards share their tails, as `q : qs` does. */
#include "arena.h"

typedef struct Boards { Cell *board; struct Boards *next; } Boards;

static int safe(long q, Cell *qs) {
  long d = 1;
  for (; qs; qs = qs->tail, d++)
    if (q == qs->head || labs(q - qs->head) == d) return 0;
  return 1;
}

int main(void) {
  long n = read_n();
  Boards *bs = alloc(sizeof *bs);
  bs->board = NULL;
  bs->next = NULL;
  for (long k = 0; k < n; k++) {
    Boards *out = NULL, **tp = &out;
    for (Boards *b = bs; b; b = b->next)
      for (long q = 1; q <= n; q++)
        if (safe(q, b->board)) {
          Boards *nb = alloc(sizeof *nb);
          nb->board = cons(q, b->board);
          nb->next = NULL;
          *tp = nb;
          tp = &nb->next;
        }
    bs = out;
  }
  long count = 0;
  for (Boards *b = bs; b; b = b->next) count++;
  printf("%ld\n", count);
}
