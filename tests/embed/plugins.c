/* plugins.c -- plugins A B FILE [CC_ARGS]: loads the modules A and B
   RTLD_LOCAL, as a plugin host does, and runs FILE in each, with PLUGIN
   set to its name. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: plugins A B FILE [CC_ARGS]\n");
    return 2;
  }
  for (int i = 0; i < 2; i++) {
    void *h = dlopen(argv[1 + i], RTLD_NOW | RTLD_LOCAL);
    int (*run)(const char *, const char *const *) = h ? (int (*)(const char *, const char *const *))dlsym(h, "plugin_run") : NULL;
    if (!run) {
      printf("%s: %s\n", argv[1 + i], dlerror());
      return 1;
    }
    const char *name = i ? "B" : "A";
    setenv("PLUGIN", name, 1);
    fflush(stdout);
    int rc = run(argv[3], (const char *const *)argv + 4);
    printf("plugin %s: %d\n", name, rc);
  }
  return 0;
}
