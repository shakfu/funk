/* host.c -- libfunk.h through the installed package; prints 55. */
#include <stdio.h>

#include "libfunk.h"

static int body(void *arg) {
  (void)arg;
  char *shown;
  if (!funk_init(NULL, 0) || !funk_eval("sum [1..10]", "<host>", &shown)) return 1;
  puts(shown);
  return 0;
}

int main(void) { return funk_main(body, NULL); }
