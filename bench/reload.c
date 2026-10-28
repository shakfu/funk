/* reload.c -- the reload threshold of docs/dev/patterns.md, over libfunk.h.
   Simulates playback in 1024-sample windows at 48 kHz (21.3 ms). Each
   window calls `step buf w` through funk_call, which writes the window's
   onsets into buf, a C array. Midway it reloads: it loads a 200-line
   pattern file and redefines `step`, then tries a file with a type error,
   which must leave the playing pattern in place. Checks that every onset
   lies in its window, in order, and that the new pattern sounds from the
   window after the reload. Reports the reload time, the slowest window
   and memory growth.

   usage: reload LIBDIR [WINDOWS] */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "libfunk.h"

enum { WINDOW = 1024, CAP = 256 };
#define STEP "step buf w = p.writeEvents (plusPtr nullPtr buf) 256 pat 96000 w (w + 1024)"

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static long rss_kib(void) {
  struct rusage u;
  getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
  return u.ru_maxrss / 1024;
#else
  return u.ru_maxrss;
#endif
}

/* n lines: n - 2 definitions with values from 1 to 59, then pat stacking 16 of them */
static void write_patterns(const char *path, int n) {
  FILE *f = fopen(path, "w");
  if (!f) { perror(path); exit(1); }
  fprintf(f, "import pattern as p\n");
  for (int i = 0; i < n - 2; i++)
    fprintf(f, "v%d = p.fast %d (p.fastcat [p.atom %d, p.silence, p.atom %d])\n", i, i % 4 + 1, 1 + i % 59, 1 + 2 * i % 59);
  fprintf(f, "pat = p.stack [");
  for (int i = 0; i < 16; i++) fprintf(f, "%sv%d", i ? ", " : "", i * (n - 2) / 16);
  fprintf(f, "]\n");
  fclose(f);
}

static bool eval(const char *src) {
  char *shown;
  bool ok = funk_eval(src, "<reload>", &shown);
  free(shown);
  return ok;
}

typedef struct { const char *lib; int windows; } Args;

static int run(void *arg) {
  Args *a = arg;
  if (!funk_init(NULL, 0)) return 1;
  funk_module_dir(a->lib);
  char dir[] = "/tmp/funk-reload-XXXXXX", good[64], bad[64];
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  snprintf(good, sizeof good, "%s/b.fk", dir);
  snprintf(bad, sizeof bad, "%s/c.fk", dir);
  write_patterns(good, 200);
  FILE *f = fopen(bad, "w");
  fprintf(f, "import pattern as p\npat = p.atom 'x' + 1\n");
  fclose(f);
  /* the first pattern's values are 60 and up, the reloaded one's below */
  if (!eval("import pattern as p") || !eval("pat = p.fastcat [p.atom 60, p.atom 64, p.atom 67]") || !eval(STEP)) return 1;

  static long buf[3 * CAP]; /* start, duration, value */
  double worst = 0, reload = 0, rejected = 0;
  long rss0 = 0, events = 0, errors = 0, last = -1;
  int half = a->windows / 2, first_new = -1;
  for (int w = 0; w < a->windows; w++) {
    if (w == half) {
      double t = now_ms();
      if (!funk_load_file(good) || !eval(STEP)) return 1;
      reload = now_ms() - t;
      t = now_ms();
      if (funk_load_file(bad)) { fputs("reload: the bad file loaded\n", stderr); return 1; }
      rejected = now_ms() - t;
    }
    long args[2] = {(long)buf, (long)w * WINDOW}, n;
    double t = now_ms();
    if (!funk_call("step", 2, args, &n)) return 1;
    t = now_ms() - t;
    if (w > 0 && t > worst) worst = t; /* the first includes warming up */
    if (w == 10) rss0 = rss_kib();
    if (n > CAP) errors++;
    for (long i = 0; i < n && i < CAP; i++) {
      long s = buf[3 * i], v = buf[3 * i + 2];
      if (s < (long)w * WINDOW || s >= (long)(w + 1) * WINDOW || s < last) errors++;
      if ((w < half) != (v >= 60)) errors++;
      if (w >= half && first_new < 0) first_new = w;
      last = s;
    }
    events += n;
  }
  printf("windows %d of %.1f ms, events %ld, errors %ld\n", a->windows, WINDOW * 1e3 / 48000, events, errors);
  printf("reload of 200 lines %.1f ms; a type error rejected in %.1f ms; new pattern from window %d of %d\n",
         reload, rejected, first_new, half);
  printf("slowest window %.3f ms\n", worst);
  printf("max rss growth after window 10: %ld KiB\n", rss_kib() - rss0);
  unlink(good);
  unlink(bad);
  rmdir(dir);
  return errors != 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { fputs("usage: reload LIBDIR [WINDOWS]\n", stderr); return 2; }
  Args a = {argv[1], argc > 2 ? atoi(argv[2]) : 2000};
  return funk_main(run, &a);
}
