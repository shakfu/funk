/* funk_output (libfunk.h) collects the program's output and messages:
   print and putStr, a compile error, a warning-free definition, trace and
   a runtime error, one message per call. With it unset, output goes to
   stdout again. The test prints what it collected; ctest compares it with
   output.expected. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfunk.h"

static char got[2][4096];
static int calls[2];

static void collect(void *ctx, int stream, const char *s, size_t n) {
  (void)ctx;
  int i = stream == 2;
  strncat(got[i], s, n < sizeof got[i] - strlen(got[i]) - 1 ? n : sizeof got[i] - strlen(got[i]) - 1);
  calls[i]++;
}

static void eval(funk *f, const char *src) {
  char *shown;
  bool ok = funk_eval(f, src, "<test>", &shown);
  printf("%s %s -> %s\n", ok ? "ok  " : "fail", src, shown ? shown : "-");
  free(shown);
}

static void dump(const char *name, int i) {
  printf("%s, %d calls:\n", name, calls[i]);
  for (char *line = strtok(got[i], "\n"); line; line = strtok(NULL, "\n")) printf("  | %s\n", line);
}

static int run(funk *f, void *arg) {
  (void)arg;
  funk_output(f, collect, NULL);
  eval(f, "print (1 + 2)");
  eval(f, "putStr \"a\" >> putStrLn \"b\"");
  eval(f, "f x = x + True");
  eval(f, "trace \"traced\" 5");
  eval(f, "head ([] :: [Int])");
  eval(f, "g x = x * 2");
  funk_output(f, NULL, NULL);
  dump("stream 1", 0);
  dump("stream 2", 1);
  eval(f, "print 7");
  return 0;
}

int main(void) {
  funk *f = funk_new(NULL);
  if (!f) return 1;
  int status = funk_run(f, run, NULL);
  funk_free(f);
  return status;
}
