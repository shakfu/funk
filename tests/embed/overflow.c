/* A stack overflow in C called from funk is the runtime error "stack
   overflow", and the session and host go on, repeatedly, and under a callback.
   Inside a system library it cannot unwind: the fatal hook runs, in a child,
   and the child exits; a hook that never returns is ended by SIGALRM. Built
   with HOST_MALLOC, the binary defines malloc, as a host linking its own
   allocator does, and an overflow in it cannot unwind either. */
#define _DEFAULT_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libfunk.h"

#ifdef HOST_MALLOC
#ifdef __APPLE__
#include <malloc/malloc.h>
void *malloc(size_t n) { return malloc_zone_malloc(malloc_default_zone(), n); }
#else
extern void *__libc_malloc(size_t n);
void *malloc(size_t n) { return __libc_malloc(n); }
#endif
#endif

static __attribute__((noinline)) long deep(long n) {
  volatile char b[256];
  b[0] = (char)n;
  return n ? deep(n - 1) + b[0] : 0;
}

/* snprintf reaches deeper than this frame, so it meets the guard first */
static __attribute__((noinline)) long deep_libc(long n) {
  char b[32];
  long r = snprintf(b, sizeof b, "%f", (double)n);
  return n ? deep_libc(n - 1) + r + b[0] : 0;
}

static void each(void *ctx, void (*fn)(void *, int)) { fn(ctx, 100000000); }

static void eval(funk *f, const char *src) {
  char *shown;
  bool ok = funk_eval(f, src, "<test>", &shown);
  printf("%s %s\n", ok ? "ok  " : "fail", shown ? shown : "-");
  fflush(stdout);
  free(shown);
}

static bool load(funk *f, const char *path) {
  return funk_provide(f, "overflow_deep", (void *)deep) && funk_provide(f, "overflow_libc", (void *)deep_libc) &&
         funk_provide(f, "overflow_each", (void *)each) && funk_load_file(f, path);
}

static int recovers(funk *f, void *path) {
  if (!load(f, path)) return 1;
  eval(f, "deep 100");
  eval(f, "deep 100000000");
  eval(f, "deep 100000000"); /* a second: the signal's mask and stack were restored */
  eval(f, "inCallback ()");
  eval(f, "sum (map (\\x -> length (reverse [1 .. 1000 + mod x 7])) [1 .. 5000])"); /* the collector, after abandoned frames */
  return 0;
}

static void on_fatal(void *ctx, const char *msg) {
  printf("hook: %s, ctx %s\n", msg, (const char *)ctx);
  fflush(stdout);
}

static void on_fatal_hangs(void *ctx, const char *msg) {
  (void)ctx, (void)msg;
  for (;;) pause();
}

static int fatal_with(funk *f, void *path, void (*hook)(void *, const char *), const char *src) {
  funk_on_fatal(f, hook, "set");
  if (!load(f, path)) return 1;
  eval(f, src);
  puts("not reached");
  fflush(stdout);
  return 0;
}

static int fatal(funk *f, void *path) { return fatal_with(f, path, on_fatal, "deepLibc 100000000"); }
static int fatal_hangs(funk *f, void *path) { return fatal_with(f, path, on_fatal_hangs, "deepLibc 100000000"); }
static int fatal_host(funk *f, void *path) { return fatal_with(f, path, on_fatal, "deep 100000000"); }

static const char *const cc_args[] = {"-Itests/embed"};

static int session(int (*fn)(funk *, void *), char *path) {
  funk *f = funk_new(&(funk_config){.stack = (size_t)64 << 20, .cc_args = cc_args, .ncc_args = 1});
  if (!f) return 1;
  int status = funk_run(f, fn, path);
  funk_free(f);
  return status;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  fflush(stdout);
#ifdef HOST_MALLOC
  return session(fatal_host, argv[1]);
#else
  pid_t child = fork();
  if (child == 0) _exit(session(fatal, argv[1]));
  int status;
  waitpid(child, &status, 0);
  printf("child exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  fflush(stdout);
  child = fork();
  if (child == 0) _exit(session(fatal_hangs, argv[1]));
  waitpid(child, &status, 0);
  printf("hanging hook ended by SIGALRM: %s\n", WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM ? "yes" : "no");
  fflush(stdout);
  return session(recovers, argv[1]);
#endif
}
