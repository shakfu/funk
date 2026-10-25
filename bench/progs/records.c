/* Particles as structs in a linked list, a new list per step, as funk's
   map over records builds one. */
#include <math.h>

#include "arena.h"

typedef struct P { double px, py, vx, vy; struct P *next; } P;

static P *mk(double px, double py, double vx, double vy, P *next) {
  P *p = alloc(sizeof *p);
  *p = (P){px, py, vx, vy, next};
  return p;
}

int main(void) {
  long n = read_n();
  P *ps = NULL, **tp = &ps;
  for (long i = 1; i <= n; i++) { *tp = mk((double)i, 0.0, 0.0, 1.0, NULL); tp = &(*tp)->next; }
  for (int k = 0; k < 100; k++) {
    P *out = NULL, **op = &out;
    for (P *p = ps; p; p = p->next) {
      *op = mk(p->px + p->vx * 0.01, p->py + p->vy * 0.01, p->vx - p->px * 0.01, p->vy - p->py * 0.01, NULL);
      op = &(*op)->next;
    }
    ps = out;
  }
  double e = 0.0;
  for (P *p = ps; p; p = p->next) e = e + p->px * p->px + p->py * p->py + p->vx * p->vx + p->vy * p->vy;
  printf("%lld\n", (long long)nearbyint(e));
}
