/* plugin.c -- a module that embeds libfunk, as a CLAP plugin does, and
   exports only plugin_run (tests/CMakeLists.txt). */
#include "libfunk.h"
#include "plugin.h"

static const char *file;

/* static: an import reaches it only through funk_provide */
static long twice(long x) { return 2 * x; }

static int body(void *arg) {
  const char *const *cc_args = arg;
  int n = 0;
  while (cc_args[n]) n++;
  if (funk_provide("plugin_twice; int x", (void *)twice) || !funk_provide("plugin_twice", (void *)twice)) return 2;
  return !(funk_init(cc_args, n) && funk_run_file(file));
}

__attribute__((visibility("default"))) int plugin_run(const char *path, const char *const *cc_args) {
  file = path;
  return funk_main(body, (void *)cc_args);
}
