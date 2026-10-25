#include "arena.h"

/* The pipeline as a C programmer writes it: one loop, no lists. */
static long round_(long n) {
  long s = 0;
  for (long i = 1; i <= n; i++)
    if (i % 2 == 0) s += i * 3;
  return s;
}

int main(void) {
  long n = read_n(), acc = 0;
  for (int k = 0; k < 20; k++) acc = (acc + round_(n)) % 1000000007;
  printf("%ld\n", acc);
}
