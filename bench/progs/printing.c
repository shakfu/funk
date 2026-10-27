/* One line per number, written with stdio, as funk's putStrLn is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"

int main(void) {
  long n = read_n();
  char buf[64];
  for (long i = 1; i <= n; i++) {
    char *p = buf;
    memcpy(p, "line ", 5);
    p = put_decimal(p + 5, i);
    *p++ = ' ';
    p = put_decimal(p, i * i);
    fwrite(buf, 1, (size_t)(p - buf), stdout);
    putchar('\n');
  }
}
