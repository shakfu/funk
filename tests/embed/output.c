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

static void eval(const char *src) {
  char *shown;
  bool ok = funk_eval(src, "<test>", &shown);
  printf("%s %s -> %s\n", ok ? "ok  " : "fail", src, shown ? shown : "-");
  free(shown);
}

static void dump(const char *name, int i) {
  printf("%s, %d calls:\n", name, calls[i]);
  for (char *line = strtok(got[i], "\n"); line; line = strtok(NULL, "\n")) printf("  | %s\n", line);
}

static int run(void *arg) {
  (void)arg;
  if (!funk_init(NULL, 0)) return 1;
  funk_output(collect, NULL);
  eval("print (1 + 2)");
  eval("putStr \"a\" >> putStrLn \"b\"");
  eval("f x = x + True");
  eval("trace \"traced\" 5");
  eval("head ([] :: [Int])");
  eval("g x = x * 2");
  funk_output(NULL, NULL);
  dump("stream 1", 0);
  dump("stream 2", 1);
  eval("print 7");
  return 0;
}

int main(void) { return funk_main(run, NULL); }
