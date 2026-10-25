#include <math.h>

#include "arena.h"

int main(void) {
  long n = read_n();
  double h = 1.0 / (double)n, acc = 0.0;
  for (long i = 0; i != n; i++) {
    double x = ((double)i + 0.5) * h;
    acc = acc + 4.0 / (1.0 + x * x);
  }
  printf("%ld\n", (long)nearbyint(acc * h * 1000000000.0));
}
