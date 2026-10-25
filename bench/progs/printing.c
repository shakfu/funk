/* One line per number, written with stdio, as funk's putStrLn is. */
#include <stdio.h>
#include <stdlib.h>

#include "arena.h"

int main(void) {
  long n = read_n();
  char buf[64];
  for (long i = 1; i <= n; i++) {
    int len = snprintf(buf, sizeof buf, "line %ld %ld", i, i * i);
    fwrite(buf, 1, (size_t)len, stdout);
    putchar('\n');
  }
}
