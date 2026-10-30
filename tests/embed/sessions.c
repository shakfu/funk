/* Sessions side by side (libfunk.h, docs/dev/embedding.md, phase 2): two
   define x differently and evaluate at once on two threads, collecting as
   they go; each one's output reaches its own callback; a StablePtr of one
   is refused by the other; they are freed out of order, and a session made
   afterwards takes a freed one's id. ctest compares the output with
   sessions.expected, also under FUNK_GCSTRESS=1. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfunk.h"

enum { ROUNDS = 20 };

typedef struct {
  funk *f;
  long x, first;
  int agree; /* rounds whose result was the first's */
  char out[2][256]; /* streams 1 and 2 */
} Side;

static Side a, b, c, d;

static void collect(void *ctx, int stream, const char *s, size_t n) {
  char *o = ((Side *)ctx)->out[stream == 2];
  strncat(o, s, n < 255 - strlen(o) ? n : 255 - strlen(o));
}

static int eval(funk *f, void *src) {
  char *shown = NULL;
  bool ok = funk_eval(f, src, "<test>", &shown);
  if (shown) printf("  %s\n", shown);
  free(shown);
  return ok ? 0 : 1;
}

/* a sum over a fresh list, so each round allocates and collects: reverse
   keeps fusion from making it a loop */
static int rounds(funk *f, void *arg) {
  Side *s = arg;
  for (int i = 0; i < ROUNDS; i++) {
    long n = 20000, r;
    if (!funk_call(f, "work", 1, &n, &r)) return 1;
    if (i == 0) s->first = r;
    s->agree += r == s->first;
  }
  return 0;
}

static void *run(void *s) {
  funk_run(((Side *)s)->f, rounds, s);
  return NULL;
}

static void make(Side *s, long x) {
  if (!(s->f = funk_new(&(funk_config){.threshold = (size_t)1 << 20}))) exit(1); /* collect often */
  s->x = x;
  funk_output(s->f, collect, s);
  char src[32];
  snprintf(src, sizeof src, "x = %ld", x);
  funk_run(s->f, eval, src);
  funk_run(s->f, eval, "work :: Int -> Int\nwork n = sum (map (\\i -> i * x) (reverse (enumFromTo 1 n)))");
}

int main(void) {
  make(&a, 1);
  make(&b, 2);
  pthread_t ta, tb;
  pthread_create(&ta, NULL, run, &a);
  pthread_create(&tb, NULL, run, &b);
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);
  printf("a: x 1, work 20000 = %ld, %d of %d rounds agree\n", a.first, a.agree, ROUNDS);
  printf("b: x 2, work 20000 = %ld, %d of %d rounds agree\n", b.first, b.agree, ROUNDS);
  funk_run(a.f, eval, "putStr \"from a\"");
  funk_run(b.f, eval, "putStr \"from b\"");
  printf("a's output \"%s\", b's \"%s\"\n", a.out[0], b.out[0]);
  /* a is the first session, so its first StablePtr is 1 << 40 | 1 */
  printf("a's own StablePtr:\n");
  funk_run(a.f, eval, "do { p <- newStablePtr (42 :: Int) ; deRefStablePtr p }");
  printf("a's StablePtr in b: %s\n",
         funk_run(b.f, eval, "deRefStablePtr (castPtrToStablePtr (plusPtr nullPtr 1099511627777) :: StablePtr Int)") ? "refused" : "accepted");
  printf("b's messages: %s", b.out[1]);
  make(&c, 3);
  funk_free(a.f); /* out of order: the first made, while b and c live */
  printf("after a's end, b:\n");
  funk_run(b.f, eval, "x + 10");
  make(&d, 4); /* a's id, free again */
  printf("d, in a's place:\n");
  funk_run(d.f, eval, "x * 100");
  funk_free(c.f);
  funk_free(b.f);
  funk_free(d.f);
  printf("all freed\n");
  return 0;
}
