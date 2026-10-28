/* funk_interrupt (libfunk.h) from another thread stops an endless loop, a
   tail-recursive one and one that grows the stack; the session goes on. */
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libfunk.h"

static void *later(void *arg) {
  (void)arg;
  usleep(100000);
  funk_interrupt();
  return NULL;
}

static void eval(const char *src) {
  pthread_t t;
  pthread_create(&t, NULL, later, NULL);
  char *shown;
  bool ok = funk_eval(src, "<test>", &shown);
  pthread_join(t, NULL);
  printf("%s %s\n", ok ? "ok  " : "fail", shown ? shown : "-");
  free(shown);
}

static int run(void *arg) {
  (void)arg;
  if (!funk_init(NULL, 0)) return 1;
  eval("let loop n = loop (n + 1) in loop 0");
  eval("let grow n = 1 + grow (mod (n + 1) 1000) in grow 0");
  eval("sum [1 .. 100]"); /* the interrupt comes while it waits: cleared by the next call */
  eval("1 + 1");
  return 0;
}

int main(void) { return funk_main(run, NULL); }
