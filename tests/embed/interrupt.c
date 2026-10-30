/* funk_interrupt (libfunk.h) from another thread stops an endless loop, a
   tail-recursive one and one that grows the stack; the session goes on. */
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libfunk.h"

static void *later(void *f) {
  usleep(100000);
  funk_interrupt(f);
  return NULL;
}

static void eval(funk *f, const char *src) {
  pthread_t t;
  pthread_create(&t, NULL, later, f);
  char *shown;
  bool ok = funk_eval(f, src, "<test>", &shown);
  pthread_join(t, NULL);
  printf("%s %s\n", ok ? "ok  " : "fail", shown ? shown : "-");
  free(shown);
}

static int run(funk *f, void *arg) {
  (void)arg;
  eval(f, "let loop n = loop (n + 1) in loop 0");
  eval(f, "let grow n = 1 + grow (mod (n + 1) 1000) in grow 0");
  eval(f, "sum [1 .. 100]"); /* the interrupt comes while it waits: cleared by the next call */
  eval(f, "1 + 1");
  return 0;
}

int main(void) {
  funk *f = funk_new(NULL);
  if (!f) return 1;
  int status = funk_run(f, run, NULL);
  funk_free(f);
  return status;
}
