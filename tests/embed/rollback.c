/* rollback A B: a failed input leaves no trace (check.c, compile_chunk).
   A's first export passes and its second fails; B, compiled next in the
   same session, exports only its own function. Before, A's first export
   stayed, naming a global index that B's definitions took. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfunk.h"

static int body(funk *f, void *arg) {
  char **files = arg;
  char *a = funk_emit_kernels(f, files[0]);
  if (a) {
    printf("%s compiled\n", files[0]);
    return 1;
  }
  char *b = funk_emit_kernels(f, files[1]);
  if (!b) return 1;
  printf("ok_three: %s, ok_one: %s\n", strstr(b, "ok_three") ? "yes" : "no", strstr(b, "ok_one") ? "yes" : "no");
  free(b);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  funk *f = funk_new(NULL);
  if (!f) return 1;
  int status = funk_run(f, body, argv + 1);
  funk_free(f);
  return status;
}
