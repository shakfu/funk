/* dsp.c -- the cost of graphs of kernels (docs/dev/audio.md, D, item 5).

   usage: dspbench KERNELS [BLOCK]

   KERNELS is lib/kernels.fk compiled (funkdsp.so). Prints ns per block of
   BLOCK frames (default 64) at 48 kHz, the best of 7 runs:
   - each funk kernel called alone;
   - the graph's cost per node: a chain of dsp_mul nodes against direct calls;
   - one voice of `d.poly` (saw, low-pass, ADSR, as `lib/dsp.fk` builds them)
     in a graph of funk kernels, of the same algorithms in C, and as one
     fused C loop; and how many voices fit in a block's time. */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "graph.h"

enum { RUNS = 7, MAX_BLOCK = 4096 };
static const double RATE = 48000, PI = 3.141592653589793; /* M_PI is POSIX, not C */
static int block = 64;
static void *lib;

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static funk_kernel kernel(const char *name) {
  funk_kernel k = (funk_kernel)dlsym(lib, name);
  if (!k) { fprintf(stderr, "dspbench: no kernel %s\n", name); exit(1); }
  return k;
}

/* best ns per call of f over enough calls for about 20 ms */
static double best(void (*f)(void *), void *arg) {
  int calls = 1;
  for (double t0 = now_ns(); now_ns() - t0 < 2e6; calls *= 2)
    for (int i = 0; i < calls; i++) f(arg);
  calls *= 10;
  double min = INFINITY;
  for (int r = 0; r < RUNS; r++) {
    double t0 = now_ns();
    for (int i = 0; i < calls; i++) f(arg);
    double t = (now_ns() - t0) / calls;
    if (t < min) min = t;
  }
  return min;
}

/* ------------------------------------------------- C kernels, as funk's */

typedef struct { double phase; } Phase;
typedef struct { double ic1, ic2; } Svf;
typedef struct { double stage, level, gate; } Env;

static void c_const(void *st, float **in, float *out, int n, const funk_context *cx) {
  (void)in, (void)cx;
  float v = (float)*(double *)st;
  for (int i = 0; i < n; i++) out[i] = v;
}

static void c_mul(void *st, float **in, float *out, int n, const funk_context *cx) {
  (void)st, (void)cx;
  for (int i = 0; i < n; i++) out[i] = in[0][i] * in[1][i];
}

static double blep(double t, double dt) {
  if (dt <= 0) return 0;
  if (t < dt) { double x = t / dt; return x + x - x * x - 1; }
  if (t > 1 - dt) { double x = (t - 1) / dt; return x * x + x + x + 1; }
  return 0;
}

static double wrap(double x) { return x >= 1 ? x - 1 : x < 0 ? x + 1 : x; }

static void c_saw(void *st, float **in, float *out, int n, const funk_context *cx) {
  Phase *s = st;
  for (int i = 0; i < n; i++) {
    double dt = in[0][i] / cx->rate, ph = s->phase;
    s->phase = wrap(ph + dt);
    out[i] = (float)(2 * ph - 1 - blep(ph, dt));
  }
}

static void c_lowpass(void *st, float **in, float *out, int n, const funk_context *cx) {
  Svf *s = st;
  double fc = fmin(in[1][0], 0.49 * cx->rate), g = tan(PI * fmax(fc, 0) / cx->rate), k = 1 / fmax(in[2][0], 0.01);
  double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
  for (int i = 0; i < n; i++) {
    double v3 = in[0][i] - s->ic2, v1 = a1 * s->ic1 + a2 * v3, v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
    s->ic1 = 2 * v1 - s->ic1;
    s->ic2 = 2 * v2 - s->ic2;
    out[i] = (float)v2;
  }
}

static double per_frame(double span, double secs, double rate) { return secs <= 0 ? 2 : span / (secs * rate); }

static double env_step(Env *s, double stage, double up, double down, double sus, double rel) {
  double l = s->level;
  s->stage = stage == 1 ? (l + up >= 1 ? 2 : 1) : stage == 3 ? (l - rel <= 0 ? 0 : 3) : stage;
  s->level = stage == 1 ? fmin(1, l + up) : stage == 2 ? fmax(sus, l - down) : stage == 3 ? fmax(0, l - rel) : 0;
  return s->level;
}

static void c_adsr(void *st, float **in, float *out, int n, const funk_context *cx) {
  Env *s = st;
  double sus = fmin(1, fmax(0, in[3][0])), up = per_frame(1, in[1][0], cx->rate);
  double down = per_frame(1 - sus, in[2][0], cx->rate), rel = per_frame(1, in[4][0], cx->rate);
  for (int i = 0; i < n; i++) {
    double g = in[0][i], stage = g > 0 && s->gate <= 0 ? 1 : g <= 0 && s->gate > 0 ? 3 : s->stage;
    s->gate = g;
    out[i] = (float)env_step(s, stage, up, down, sus, rel);
  }
}

/* -------------------------------------------------------- one kernel alone */

typedef struct {
  funk_kernel k;
  double state[3];
  float *in[5];
  float out[MAX_BLOCK];
  funk_context cx;
} Call;

static float ones[MAX_BLOCK], freqs[MAX_BLOCK], noise[MAX_BLOCK], gate[MAX_BLOCK], tiny[MAX_BLOCK];

static void call(void *arg) {
  Call *c = arg;
  c->k(c->state, c->in, c->out, block, &c->cx);
}

static void kernels(void) {
  struct { const char *name; float *in[5]; } ks[] = {
      {"dsp_const", {0}},
      {"dsp_add", {noise, noise}},
      {"dsp_mul", {noise, noise}},
      {"dsp_sine", {freqs}},
      {"dsp_saw", {freqs}},
      {"dsp_square", {freqs, ones}},
      {"dsp_lowpass1", {noise, freqs}},
      {"dsp_lowpass", {noise, freqs, ones}},
      {"dsp_adsr", {gate, tiny, tiny, ones, tiny}},
  };
  printf("one kernel, ns per block:\n");
  for (size_t i = 0; i < sizeof ks / sizeof *ks; i++) {
    Call c = {kernel(ks[i].name), {0.5}, {0}, {0}, {RATE}};
    memcpy(c.in, ks[i].in, sizeof c.in);
    printf("  %-13s %7.1f\n", ks[i].name, best(call, &c));
  }
}

/* ------------------------------------------------------------- graphs */

typedef struct {
  funk_player *p;
  float out[MAX_BLOCK];
} Play;

static void render(void *arg) {
  Play *pl = arg;
  funk_player_render(pl->p, pl->out, block);
}

/* ns per block of g, played alone */
static double graph_ns(funk_graph *g) {
  Play pl = {funk_player_new(), {0}};
  funk_player_play(pl.p, g);
  double t = best(render, &pl);
  funk_player_free(pl.p);
  return t;
}

static int add(funk_graph *g, funk_kernel k, double s0, int size, const int *ins, int nin) {
  double st[3] = {s0, 0, 0};
  int i = funk_graph_add(g, k, st, (size_t)size * sizeof(double), ins, nin);
  if (i < 0) { fputs("dspbench: out of memory\n", stderr); exit(1); }
  return i;
}

/* n dsp_mul nodes in a chain, each of the last and a constant */
static void per_node(void) {
  enum { N = 256 };
  funk_kernel mul = kernel("dsp_mul"), cst = kernel("dsp_const");
  funk_graph *g = funk_graph_new(MAX_BLOCK, RATE);
  int one = add(g, cst, 1.0, 1, NULL, 0), last = one;
  for (int i = 0; i < N; i++) last = add(g, mul, 0, 0, (int[]){last, one}, 2);
  double graph = graph_ns(g) / N;
  Call c = {mul, {0}, {noise, noise}, {0}, {RATE}};
  double direct = best(call, &c);
  printf("graph cost per node, %d dsp_mul in a chain, ns per block:\n", N);
  printf("  in the graph  %7.1f\n  called alone  %7.1f\n  difference    %7.1f\n", graph, direct, graph - direct);
}

typedef struct { funk_kernel cst, saw, lowpass, adsr, mul; } Kit;

/* d.poly's voice: lowpass 2000 (dc 0.7) (saw freq) * adsr a d s r gate,
   with gate and freq controls; nodes in lib/dsp.fk's order */
static int voice(funk_graph *g, const Kit *k) {
  int gt = funk_graph_control(g, 1.0f), fr = funk_graph_control(g, 220.0f);
  int saw = add(g, k->saw, 0, 1, &fr, 1);
  int cut = add(g, k->cst, 2000, 1, NULL, 0), q = add(g, k->cst, 0.7, 1, NULL, 0);
  int lp = add(g, k->lowpass, 0, 2, (int[]){saw, cut, q}, 3);
  int a = add(g, k->cst, 0.01, 1, NULL, 0), d = add(g, k->cst, 0.2, 1, NULL, 0);
  int s = add(g, k->cst, 0.5, 1, NULL, 0), r = add(g, k->cst, 0.3, 1, NULL, 0);
  int env = add(g, k->adsr, 0, 3, (int[]){gt, a, d, s, r}, 5);
  return add(g, k->mul, 0, 0, (int[]){lp, env}, 2);
}

static double voices_ns(const Kit *k, int n) {
  funk_graph *g = funk_graph_new(MAX_BLOCK, RATE);
  int sum = voice(g, k);
  for (int i = 1; i < n; i++) sum = add(g, kernel("dsp_add"), 0, 0, (int[]){sum, voice(g, k)}, 2);
  return graph_ns(g);
}

/* the voice as one loop, as a compiler fusing the graph would write it */
typedef struct { Phase osc; Svf lp; Env env; float out[MAX_BLOCK]; } Fused;

static void fused(void *arg) {
  Fused *v = arg;
  double fc = 2000, g = tan(PI * fc / RATE), k = 1 / 0.7, a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
  double sus = 0.5, up = per_frame(1, 0.01, RATE), down = per_frame(1 - sus, 0.2, RATE), rel = per_frame(1, 0.3, RATE);
  for (int i = 0; i < block; i++) {
    double dt = 220 / RATE, ph = v->osc.phase;
    v->osc.phase = wrap(ph + dt);
    double x = 2 * ph - 1 - blep(ph, dt);
    double v3 = x - v->lp.ic2, v1 = a1 * v->lp.ic1 + a2 * v3, v2 = v->lp.ic2 + a2 * v->lp.ic1 + a3 * v3;
    v->lp.ic1 = 2 * v1 - v->lp.ic1;
    v->lp.ic2 = 2 * v2 - v->lp.ic2;
    double stage = v->env.gate <= 0 ? 1 : v->env.stage;
    v->env.gate = 1;
    v->out[i] = (float)(v2 * env_step(&v->env, stage, up, down, sus, rel));
  }
}

static void voices(void) {
  Kit fk = {kernel("dsp_const"), kernel("dsp_saw"), kernel("dsp_lowpass"), kernel("dsp_adsr"), kernel("dsp_mul")};
  Kit ck = {c_const, c_saw, c_lowpass, c_adsr, c_mul};
  double budget = block / RATE * 1e9;
  printf("one voice of d.poly (12 nodes), ns per block; voices in %.0f ns:\n", budget);
  struct { const char *what; double ns; } rows[3];
  double f64 = voices_ns(&fk, 64) / 64, c64 = voices_ns(&ck, 64) / 64;
  Fused v = {{0}, {0, 0}, {0, 0, 0}, {0}};
  rows[0].what = "funk kernels", rows[0].ns = f64;
  rows[1].what = "C kernels", rows[1].ns = c64;
  rows[2].what = "fused C", rows[2].ns = best(fused, &v);
  for (int i = 0; i < 3; i++) printf("  %-13s %7.1f %6.0f\n", rows[i].what, rows[i].ns, budget / rows[i].ns);
  printf("  (graphs of 64 voices summed; 1 voice of funk kernels alone: %.1f)\n", voices_ns(&fk, 1));
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    fputs("usage: dspbench KERNELS [BLOCK]\n", stderr);
    return 2;
  }
  if (argc == 3) block = atoi(argv[2]);
  if (block < 1 || block > MAX_BLOCK) {
    fprintf(stderr, "dspbench: BLOCK must be 1 to %d\n", MAX_BLOCK);
    return 2;
  }
  if (!(lib = dlopen(argv[1], RTLD_NOW))) {
    fprintf(stderr, "dspbench: %s\n", dlerror());
    return 1;
  }
  for (int i = 0; i < MAX_BLOCK; i++) {
    ones[i] = 1, freqs[i] = 220, gate[i] = 1, tiny[i] = 0.01f;
    noise[i] = (float)((i * 7919 % 2003) / 1001.5 - 1);
  }
  printf("block %d frames at %.0f Hz\n", block, RATE);
  kernels();
  per_node();
  voices();
  return 0;
}
