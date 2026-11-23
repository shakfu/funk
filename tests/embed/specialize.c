/* specialize FILE: the copy budget of specialization on function arguments
   (elab.c, HO_MAX_COPIES) is per input, not per session. 3000 inputs, each
   making new copies, pass the budget; FILE's report still names a copy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfunk.h"

static int body(funk *f, void *arg) {
  char *shown, line[160];
  if (!funk_eval(f, "twice g x = g (g x)", "<specialize>", &shown)) return 1;
  free(shown);
  for (int i = 0; i < 3000; i++) {
    snprintf(line, sizeof line, "map (twice (\\x -> x + %d)) [1, 2]", i);
    if (!funk_eval(f, line, "<specialize>", &shown)) return 1;
    free(shown);
  }
  char *report = funk_opt_report(f, arg);
  if (!report) return 1;
  printf("%s\n", strstr(report, "apply@{") ? "specialized after 3000 inputs" : report);
  free(report);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  funk *f = funk_new(NULL);
  if (!f) return 1;
  int status = funk_run(f, body, argv[1]);
  funk_free(f);
  return status;
}
