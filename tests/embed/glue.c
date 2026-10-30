/* glue.c -- funk_glue: linked.fk's import, its glue linked in by
   `funk emit-c --glue` (tests/CMakeLists.txt), runs with no C compiler.
   With --no-glue it is compiled with $CC, as before. */
#include <stdio.h>
#include <string.h>

#include "libfunk.h"
#include "linked.h"

extern const funk_glue_entry funk_glue_table[];
extern const int funk_glue_count;

int64_t linked_twice(int64_t x) { return 2 * x; }

static const char *dir;
static int use_glue;

static int body(funk *f, void *arg) {
  (void)arg;
  if (use_glue) funk_glue(f, funk_glue_table, funk_glue_count);
  funk_module_dir(f, dir);
  char *shown = NULL;
  /* a global first, so the import's slot differs from emit-c's */
  if (!funk_eval(f, "x = 1", "<glue>", &shown) || !funk_eval(f, "import linked as l", "<glue>", &shown) ||
      !funk_eval(f, "l.twice 21", "<glue>", &shown))
    return 1;
  printf("%s\n", shown);
  return 0;
}

int main(int argc, char **argv) {
  dir = argv[1];
  use_glue = !(argc > 2 && !strcmp(argv[2], "--no-glue"));
  funk *f = funk_new(NULL);
  if (!f) return 1;
  int status = funk_run(f, body, NULL);
  funk_free(f);
  return status;
}
