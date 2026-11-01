/* kernels.c -- funk_compile_kernels: a library for dlopen, cached by source, with
   the compiler's messages in funk_output; and a new session for each of many compiles. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libfunk.h"

static char messages[8192];
static char path[512];

static void output(void *ctx, int stream, const char *s, size_t n) {
  (void)ctx, (void)stream;
  size_t have = strlen(messages);
  if (have + n < sizeof messages) memcpy(messages + have, s, n), messages[have + n] = 0;
}

static const char *source;
static char *library;

static int compile(funk *f, void *arg) {
  (void)arg;
  funk_output(f, output, NULL);
  FILE *file = fopen(path, "w");
  if (!file) return 1;
  fputs(source, file);
  fclose(file);
  library = funk_compile_kernels(f, path);
  unlink(path);
  return 0;
}

/* the library for src, or NULL with its messages in `messages` */
static char *build(const char *src) {
  funk *f = funk_new(NULL);
  if (!f) {
    puts("no session");
    exit(1);
  }
  source = src;
  library = NULL;
  messages[0] = 0;
  funk_run(f, compile, NULL);
  funk_free(f);
  return library;
}

#define GAIN(name, k)                                                 \
  "foreign export ccall \"" name "\" f :: Int -> Int\n"               \
  "f :: Int -> Int\n"                                                 \
  "f x = x * " k "\n"

int main(void) {
  snprintf(path, sizeof path, "/tmp/funk-embed-kernels-%d.fk", (int)getpid());

  /* compiles, loads, and runs */
  char *so = build(GAIN("k_gain", "3"));
  if (!so) return printf("failed: %s\n", messages), 1;
  void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
  long (*gain)(long) = lib ? (long (*)(long))dlsym(lib, "k_gain") : NULL;
  printf("gain: %ld\n", gain ? gain(7) : -1);

  /* the same source is the same file; another is another */
  char *same = build(GAIN("k_gain", "3")), *other = build(GAIN("k_gain", "4"));
  printf("same source, same library: %d\n", same && !strcmp(so, same));
  printf("new source, new library: %d\n", other && strcmp(so, other) != 0);
  void *lib2 = other ? dlopen(other, RTLD_NOW | RTLD_LOCAL) : NULL;
  long (*gain2)(long) = lib2 ? (long (*)(long))dlsym(lib2, "k_gain") : NULL;
  printf("both load, each its own: %ld %ld\n", gain(7), gain2 ? gain2(7) : -1);

  /* Float primitives need math.h, and no import names it */
  char *prims = build("foreign export ccall \"k_prims\" f :: Float -> Float\nf :: Float -> Float\nf x = abs (sqrt (exp x) + sin x) - x\n");
  void *lib3 = prims ? dlopen(prims, RTLD_NOW | RTLD_LOCAL) : NULL;
  double (*fp)(double) = lib3 ? (double (*)(double))dlsym(lib3, "k_prims") : NULL;
  printf("Float primitives: %.3f\n", fp ? fp(0.0) : -1.0);

  /* funk's errors */
  char *bad = build("foreign export ccall \"k\" f :: Int -> String\nf :: Int -> String\nf x = show x\n");
  printf("a heap result: %s, %s\n", bad ? "compiled" : "refused", strstr(messages, "`String`") ? "says why" : messages);

  /* the C compiler's: an import that C does not have */
  bad = build("foreign export ccall \"k\" f :: Float -> Float\nforeign import ccall \"math.h no_such_function\" g :: Float -> Float\nf :: Float -> Float\nf x = g x\n");
  printf("a missing C function: %s, %s\n", bad ? "compiled" : "refused", strstr(messages, "no_such_function") ? "shows the compiler's message" : messages);

  /* no compiler */
  setenv("CC", "/nonexistent/cc", 1);
  bad = build(GAIN("k_gain", "5"));
  printf("no compiler: %s, %s\n", bad ? "compiled" : "refused", strstr(messages, "C compiler") ? "says so" : messages);
  unsetenv("CC");

  /* an evaluation session's limit does not meet a host that compiles often */
  int ok = 0;
  for (int i = 0; i < 300; i++) {
    char *again = build(GAIN("k_gain", "3"));
    ok += again != NULL;
    free(again);
  }
  printf("300 sessions compile: %d\n", ok);
  return 0;
}
