/* plugin.c -- a module that embeds libfunk, as a CLAP plugin does, and
   exports only plugin_run (tests/CMakeLists.txt). */
#include "libfunk.h"
#include "plugin.h"

/* static: an import reaches it only through funk_provide */
static long twice(long x) { return 2 * x; }

static int body(funk *f, void *arg) {
  const char *file = arg;
  if (funk_provide(f, "plugin_twice; int x", (void *)twice) || !funk_provide(f, "plugin_twice", (void *)twice)) return 2;
  return !funk_run_file(f, file);
}

__attribute__((visibility("default"))) int plugin_run(const char *path, const char *const *cc_args) {
  int n = 0;
  while (cc_args[n]) n++;
  funk *f = funk_new(&(funk_config){.cc_args = cc_args, .ncc_args = n});
  if (!f) return 1;
  int status = funk_run(f, body, (void *)path);
  funk_free(f);
  return status;
}
