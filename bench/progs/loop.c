#include "arena.h"

int main(void) {
  long acc = 0;
  for (long i = read_n(); i != 0; i--) acc += i % 7;
  printf("%ld\n", acc);
}
