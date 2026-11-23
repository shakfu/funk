/* compiler_bug FILE: a broken invariant in the compiler is a compile error
   (funk.h, BUG), and the session goes on. Linked with libfunk_hooks, whose
   compiler calls BUG for a binding named fkTestBug, in fusion inside
   compile_chunk, and for a program defining fkTestCgenBug, in code
   generation. The failed input leaves nothing behind. */
#include <stdio.h>
#include <stdlib.h>

#include "libfunk.h"

static void eval(funk *f, const char *src) {
  char *shown = NULL;
  bool ok = funk_eval(f, src, "<test>", &shown);
  fflush(stderr);
  printf("%s %s\n", ok ? "ok  " : "fail", shown ? shown : "-");
  fflush(stdout);
  free(shown);
}

static int run(funk *f, void *arg) {
  eval(f, "x = 5");
  eval(f, "fkTestBug = x + 1");
  eval(f, "fkTestBug");
  eval(f, "x * 2");
  char *c = funk_emit_c(f, arg);
  fflush(stderr);
  printf("emit-c: %s\n", c ? "code" : "none");
  free(c);
  eval(f, "x + 1");
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  funk *f = funk_new(NULL);
  if (!f) return 1;
  int status = funk_run(f, run, argv[1]);
  funk_free(f);
  return status;
}
