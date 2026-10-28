/* funk_memory and funk_on_fatal (libfunk.h). With a 64 MiB heap, a list
   that stays live past the limit is the runtime error "out of memory", and
   the session goes on; a computation whose garbage exceeds the limit many
   times over runs, with the process's peak memory, until then, under
   100 MiB; deep
   recursion on a 64 MiB stack is "stack overflow". The fatal hook runs in a
   child, before it exits. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libfunk.h"
#include "rt.h" /* fk_fatal, to reach the hook without exhausting memory */

static void eval(const char *src) {
  char *shown;
  bool ok = funk_eval(src, "<test>", &shown);
  printf("%s %s\n", ok ? "ok  " : "fail", shown ? shown : "-");
  fflush(stdout);
  free(shown);
}

static long peak_mib(void) {
  struct rusage u;
  getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
  return u.ru_maxrss >> 20;
#else
  return u.ru_maxrss >> 10;
#endif
}

static int run(void *arg) {
  (void)arg;
  if (!funk_init(NULL, 0)) return 1;
  eval("let build n acc = if n == 0 then acc else build (n - 1) (n : acc) in length (build 10000000 [])");
  eval("sum (map (\\x -> length (reverse [1 .. 1000 + mod x 7])) [1 .. 5000])");
  printf("peak under 100 MiB: %s\n", peak_mib() < 100 ? "yes" : "no"); /* before the stack fills */
  eval("let f n = if n == 0 then 0 else 1 + f (n - 1) in f 100000000");
  eval("1 + 1");
  return 0;
}

static void on_fatal(void *ctx, const char *msg) {
  printf("hook: %s, ctx %s\n", msg, (const char *)ctx);
  fflush(stdout);
}

int main(void) {
  pid_t child = fork();
  if (child == 0) {
    funk_on_fatal(on_fatal, "set");
    fk_fatal("test");
  }
  int status;
  waitpid(child, &status, 0);
  printf("child exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  funk_memory((size_t)64 << 20, 0, (size_t)64 << 20);
  return funk_main(run, NULL);
}
