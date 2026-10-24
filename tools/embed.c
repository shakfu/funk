/* embed SYMBOL FILE [SYMBOL FILE ...] -- write C arrays holding each FILE,
   with local `#include "x"` lines expanded so the runtime can be pasted
   into generated programs as one translation unit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void emit(FILE *out, const char *path, int depth) {
  FILE *f = fopen(path, "r");
  if (!f || depth > 8) { fprintf(stderr, "embed: cannot read %s\n", path); exit(1); }
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    char inc[1024];
    if (sscanf(line, " #include \"%1000[^\"]\"", inc) == 1) {
      char sub[2048];
      const char *slash = strrchr(path, '/');
      snprintf(sub, sizeof sub, "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, inc);
      emit(out, sub, depth + 1);
      continue;
    }
    for (char *p = line; *p; p++) fprintf(out, "%d,", (unsigned char)*p);
    fputc('\n', out);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc < 3 || argc % 2 == 0) { fputs("usage: embed SYMBOL FILE ...\n", stderr); return 2; }
  for (int i = 1; i < argc; i += 2) {
    printf("const char %s[] = {\n", argv[i]);
    emit(stdout, argv[i + 1], 0);
    puts("0};");
  }
  return 0;
}
