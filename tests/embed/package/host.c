/* host.c -- libfunk.h through the installed package; prints 55. */
#include <stdio.h>

#include "libfunk.h"

static int body(funk *f, void *arg) {
  (void)arg;
  char *shown;
  if (!funk_eval(f, "sum [1..10]", "<host>", &shown)) return 1;
  puts(shown);
  return 0;
}

int main(void) {
  funk *f = funk_new(NULL);
  if (!f) return 1;
  int status = funk_run(f, body, NULL);
  funk_free(f);
  return status;
}
