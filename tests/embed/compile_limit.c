/* A compile that exceeds FK_COMPILE_LIMIT fails with an error, and the session
   goes on: the limit is tripped at many points of one compile, so in every
   pass, and each time the session still compiles and runs new code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libfunk.h"

/* classes, fusion, lifting, specialization and cell reuse each have work here */
static const char *big =
  "data T = L | N T Int T deriving (Eq, Show)\n"
  "class Size a where\n"
  "  size :: a -> Int\n"
  "instance Size T where\n"
  "  size L = 0\n"
  "  size (N l _ r) = size l + 1 + size r\n"
  "ins x L = N L x L\n"
  "ins x (N l y r) = if x < y then N (ins x l) y r else N l y (ins x r)\n"
  "twice f x = f (f x)\n"
  "nest :: Int -> (Int -> Int) -> Int\n"
  "nest n f = if n == 0 then f 1 else nest (n - 1) (\\x -> f (x + 1))\n"
  "total n = foldl (+) 0 (map (* 2) (filter (> 1) [1 .. n]))\n"
  "go n = let h k = k + n in twice h (nest 3 (* 2)) + total n + size (ins n (ins 1 L))\n";

static bool eval(funk *f, const char *src, char **shown) {
  return funk_eval(f, src, "<test>", shown);
}

static int sweep(funk *f, void *arg) {
  (void)arg;
  int failed = 0;
  bool passed = false;
  char *shown = NULL;
  for (long limit = 200; !passed && limit < 40000000; limit += limit / 100 + 16) {
    char s[32];
    snprintf(s, sizeof s, "%ld", limit);
    setenv("FK_COMPILE_LIMIT", s, 1);
    if (eval(f, big, &shown)) passed = true; /* its class is defined now: the last */
    else failed++;
    unsetenv("FK_COMPILE_LIMIT");
    free(shown);
    shown = NULL;
    bool ok = eval(f, "again g x = g (g x)\nafter = again (+ 1) (foldl (+) 0 (map (* 2) [1, 2, 3]))", &shown);
    free(shown);
    shown = NULL;
    if (ok) ok = eval(f, "after", &shown) && shown && !strcmp(shown, "14");
    if (!ok) {
      printf("after limit %ld: got %s\n", limit, shown ? shown : "-");
      return 1;
    }
    free(shown);
    shown = NULL;
  }
  /* both outcomes, or the sweep tested nothing */
  if (failed < 100 || !passed) {
    printf("%d failed, %s\n", failed, passed ? "then passed" : "never passed");
    return 1;
  }
  puts("ok");
  return 0;
}

int main(void) {
  funk *f = funk_new(NULL);
  if (!f) return 1;
  int r = funk_run(f, sweep, NULL);
  funk_free(f);
  return r;
}
