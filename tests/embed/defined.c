/* funk_defined (libfunk.h) after each input: a declaration binds its names,
   an expression or a failed input binds none, and a loaded file binds its
   own names, not those of its imports (pattern defines sig). */
#include <stdio.h>
#include <stdlib.h>

#include "libfunk.h"

static const char *const names[] = {"pat", "sig", "x", "y"};

static void report(funk *f, const char *what) {
  printf("%s ->", what);
  for (int i = 0; i < 4; i++)
    if (funk_defined(f, names[i])) printf(" %s", names[i]);
  printf("\n");
}

static void eval(funk *f, const char *src) {
  char *shown;
  funk_eval(f, src, "<test>", &shown);
  free(shown);
  printf("[");
  for (const char *c = src; *c; c++) fputs(*c == '\n' ? "; " : (char[]){*c, 0}, stdout);
  printf("]");
  report(f, "");
}

static int run(funk *f, void *arg) {
  funk_module_dir(f, "lib");
  eval(f, "x = 1");
  eval(f, "x + 1");
  eval(f, "x = 2\ny = 3");
  eval(f, "y = True + 1");
  eval(f, "sig = 4");
  eval(f, "(x, pat) = (5, 6)");
  eval(f, "import pattern (..)");
  funk_load_file(f, arg);
  report(f, "[load]");
  return 0;
}

int main(int argc, char **argv) {
  funk *f = funk_new(NULL);
  if (!f || argc < 2) return 1;
  int status = funk_run(f, run, argv[1]);
  funk_free(f);
  return status;
}
