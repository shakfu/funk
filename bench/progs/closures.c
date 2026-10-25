/* Closures as code pointer plus environment, called indirectly. */
#include "arena.h"

typedef struct Fn { long (*code)(struct Fn *, long); long a; struct Fn *f, *g; } Fn;

static long lam(Fn *self, long x) {
  (void)self;
  return (x * 31 + 7) % 1000003;
}
static long step(Fn *self, long x) { return (x * self->a + 7) % 1000003; }
static long compose(Fn *self, long x) { return self->f->code(self->f, self->g->code(self->g, x)); }

static long apply_n(Fn *f, long n, long x) {
  for (; n != 0; n--) x = f->code(f, x);
  return x;
}

int main(void) {
  long n = read_n();
  Fn s3 = {step, 3, 0, 0}, s5 = {step, 5, 0, 0};
  Fn fs[] = {{lam, 0, 0, 0}, {step, 17, 0, 0}, {compose, 0, &s3, &s5}};
  long acc = 0;
  for (int i = 0; i < 3; i++) acc += apply_n(&fs[i], n, 1);
  printf("%ld\n", acc);
}
