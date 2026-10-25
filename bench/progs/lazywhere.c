/* C has no thunk to allocate: this measures funk's cost for laziness. */
#include "arena.h"

int main(void) {
  long acc = 0;
  for (long i = read_n(); i != 0; i--) {
    long y = i % 7;
    acc += y;
  }
  printf("%ld\n", acc);
}
