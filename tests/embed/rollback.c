/* funk_eval frees a simple expression with a scalar result once it has run
   (docs/dev/patterns.md, friction 6). `it` and earlier definitions survive,
   anything that may keep the expression's code is kept, and 200,000 inputs
   stay well under what keeping them all would use (7 KB each, 1.4 GB). */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "libfunk.h"

static void eval(funk *f, const char *src) {
  char *shown;
  bool ok = funk_eval(f, src, "<test>", &shown);
  printf("%s: %s %s\n", src, ok ? "ok" : "fail", shown ? shown : "-");
  free(shown);
}

static int body(funk *f, void *arg) {
  (void)arg;
  char *shown;
  eval(f, "1 + 2");
  eval(f, "it + 1"); /* `it` survives, and its type */
  eval(f, "2.5 * 2.0");
  eval(f, "it + 0.5"); /* `it` changed type */
  eval(f, "not True");
  eval(f, "it");
  eval(f, "add a b = a + b");
  eval(f, "add 3 4");
  eval(f, "add 3"); /* a partial application: a function, so kept */
  eval(f, "it 10");
  eval(f, "[1, 2]"); /* not scalar: kept */
  eval(f, "length it");
  eval(f, "nosuch 1"); /* a compile error, then a runtime one, then normal */
  eval(f, "1 + 2");
  eval(f, "div 1 0");
  eval(f, "it");
  eval(f, "1 + 1");
  for (int i = 0; i < 200000; i++) {
    if (!funk_eval(f, "1 + 2", "<test>", &shown)) return 1;
    free(shown);
  }
  /* definitions made after many rollbacks still work */
  eval(f, "sub a b = a - b");
  eval(f, "sub 5 3");
  eval(f, "it + add 1 1");
  struct rusage u;
  getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
  long mib = u.ru_maxrss >> 20;
#else
  long mib = u.ru_maxrss >> 10;
#endif
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
  mib = 0; /* its shadow memory and quarantine count as resident */
#endif
  printf("under 900 MiB: %d\n", mib < 900);
  return 0;
}

int main(void) {
  funk *f = funk_new(NULL);
  int rc = funk_run(f, body, NULL);
  funk_free(f);
  return rc;
}
