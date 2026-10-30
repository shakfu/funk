/* funk_new, funk_run and funk_free (libfunk.h). Jobs from two threads run
   on the session's thread, one at a time; a job's own funk_run runs at
   once. A call off the session's thread and funk_free inside a job fail
   with a message; a second session works beside the first. ctest compares the output with
   session.expected. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfunk.h"

enum { JOBS = 1000 };

static char msgs[1024];
static long count;
static pthread_t first;
static int elsewhere; /* jobs not on the first job's thread */

static void collect(void *ctx, int stream, const char *s, size_t n) {
  (void)ctx, (void)stream;
  strncat(msgs, s, n < sizeof msgs - strlen(msgs) - 1 ? n : sizeof msgs - strlen(msgs) - 1);
}

static int show(funk *f, void *src) {
  char *shown = NULL;
  bool ok = funk_eval(f, src, "<test>", &shown);
  printf("%s\n", !ok ? "fail" : shown ? shown : "ok");
  free(shown);
  return ok ? 0 : 1;
}

static int nested(funk *f, void *src) { return funk_run(f, show, src); }

/* no lock: jobs run one at a time */
static int step(funk *f, void *arg) {
  (void)arg;
  if (!count) first = pthread_self();
  else if (!pthread_equal(first, pthread_self())) elsewhere++;
  long r;
  if (!funk_call(f, "inc", 1, &count, &r)) return 1;
  count = r;
  return 0;
}

static void *worker(void *f) {
  for (int i = 0; i < JOBS; i++) funk_run(f, step, NULL);
  return NULL;
}

static int free_inside(funk *f, void *arg) {
  (void)arg;
  funk_free(f);
  return 0;
}

int main(void) {
  funk *f = funk_new(NULL);
  if (!f) return 1;
  funk_output(f, collect, NULL);
  printf("first: ");
  funk_run(f, show, "sum [1 .. 10]");
  char *shown = NULL;
  printf("off its thread: %s\n", funk_eval(f, "1", "<test>", &shown) ? "ok" : "fail");
  funk *g = funk_new(NULL);
  printf("second session: ");
  if (g) funk_run(g, show, "sum [1 .. 4]");
  funk_free(g);
  printf("nested: ");
  funk_run(f, nested, "1 + 5");
  printf("inc defined: ");
  funk_run(f, show, "inc :: Int -> Int\ninc x = x + 1");
  pthread_t a, b;
  pthread_create(&a, NULL, worker, f);
  pthread_create(&b, NULL, worker, f);
  pthread_join(a, NULL);
  pthread_join(b, NULL);
  printf("2 threads x %d jobs: %ld, all on one thread: %s\n", JOBS, count, elsewhere ? "no" : "yes");
  funk_run(f, free_inside, NULL);
  funk_free(f);
  printf("messages:\n%s", msgs);
  return 0;
}
