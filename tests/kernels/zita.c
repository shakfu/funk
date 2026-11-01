/* lib/kernels.fk's dsp_zita against Faust, at mix 1. zita.expected holds Faust 2.88's
   re.zita_rev1_stereo(60, 200, 6000, 3, 2, 48000), compiled with -double and run
   by its interpreter, on zita_stimulus.h: every 4000th frame, 7 decimals. The
   kernels print the same. Then the same kernels as two nodes of a graph, which is
   how a patch plays them: both channels must equal a direct call, sample by sample. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graph.h"
#include "zita_stimulus.h"

static float l[ZITA_FRAMES], r[ZITA_FRAMES], out[2][ZITA_FRAMES];
static funk_kernel zita;

static void input_kernel(void *st, float **in, float *o, int n, const funk_context *cx) {
  (void)in, (void)cx;
  long long *at = st;
  const float *src = (const float *)(intptr_t)at[1];
  for (int i = 0; i < n; i++) o[i] = src[at[0]++];
}

/* A 220 Hz tone through the reverb; at 2 s, parameter p jumps to `to`. Returns the
   largest second difference of the output in the next 0.5 s, which a click raises and
   a glide does not; *before is the same in the 0.5 s before. The state is left in st. */
static double jump(double *st, float *mem, int p, float to, double *before) {
  static float params[6][64], x[64], o[64];
  float *in[8] = {x, x, params[0], params[1], params[2], params[3], params[4], params[5]};
  float v[6] = {0.06f, 200, 6000, 3, 2, 1};
  funk_context cx = {ZITA_RATE};
  long long frames = 4 * ZITA_RATE + 2;
  memset(st, 0, 145 * sizeof *st);
  memset(mem, 0, (size_t)frames * sizeof *mem);
  memcpy(&st[0], &mem, sizeof mem);
  memcpy(&st[1], &frames, sizeof frames);
  double phase = 0, last = 0, slope = 0, after = 0;
  int change = 2 * ZITA_RATE / 64, span = ZITA_RATE / 128;
  *before = 0;
  for (int b = 0; b < change + 6 * span; b++) {
    if (b == change && p >= 0) v[p] = to;
    for (int k = 0; k < 6; k++)
      for (int i = 0; i < 64; i++) params[k][i] = v[k];
    for (int i = 0; i < 64; i++) x[i] = (float)(0.5 * sin(phase)), phase += 2 * M_PI * 220.0 / ZITA_RATE;
    zita(st, in, o, 64, &cx);
    for (int i = 0; i < 64; i++) {
      double s = o[i] - last, d = fabs(s - slope);
      last = o[i], slope = s;
      if (b >= change - span && b < change && d > *before) *before = d;
      if (b >= change && b < change + span && d > after) after = d;
    }
  }
  return after;
}

/* line coefficients and filter coefficients, compared in slots */
static int settled(const double *a, const double *b) {
  if (a[9] != b[9] || a[10] != b[10] || a[11] != b[11] || a[13] != 0) return 0;
  for (int i = 0; i < 8; i++)
    for (int f = 10; f <= 12; f++)
      if (a[17 + 16 * i + f] != b[17 + 16 * i + f]) return 0;
  return 1;
}

int main(int argc, char **argv) {
  void *lib = argc > 1 ? dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) : NULL;
  zita = lib ? (funk_kernel)dlsym(lib, "dsp_zita") : NULL;
  if (!zita) return 1;
  zita_stimulus(l, r);
  /* direct calls */
  static double st[2][145];
  static float params[6][64];
  static const float values[6] = {0.06f, 200, 6000, 3, 2, 1};
  for (int p = 0; p < 6; p++)
    for (int i = 0; i < 64; i++) params[p][i] = values[p];
  long long frames = 4 * ZITA_RATE + 2;
  funk_context cx = {ZITA_RATE};
  for (int c = 0; c < 2; c++) {
    float *mem = calloc((size_t)frames, sizeof *mem);
    memcpy(&st[c][0], &mem, sizeof mem);
    memcpy(&st[c][1], &frames, sizeof frames);
    st[c][2] = c;
    for (int i = 0; i < ZITA_FRAMES; i += 64) {
      float *in[8] = {l + i, r + i, params[0], params[1], params[2], params[3], params[4], params[5]};
      zita(st[c], in, out[c] + i, 64, &cx);
    }
    free(mem);
  }
  for (int i = 0; i < ZITA_FRAMES; i += 4000) printf("%6d %.7f %.7f\n", i, out[0][i], out[1][i]);

  /* a graph of two nodes: its memory comes from funk_graph_memory */
  funk_graph *g = funk_graph_new(64, ZITA_RATE);
  long long sl[2] = {0, (long long)(intptr_t)l}, sr[2] = {0, (long long)(intptr_t)r};
  int in[8] = {funk_graph_add(g, input_kernel, sl, sizeof sl, NULL, 0), funk_graph_add(g, input_kernel, sr, sizeof sr, NULL, 0)};
  for (int p = 0; p < 6; p++) in[2 + p] = funk_graph_control(g, values[p]);
  for (int c = 0; c < 2; c++) {
    double zero[145] = {0};
    zero[2] = c;
    int n = funk_graph_add(g, zita, zero, sizeof zero, in, 8);
    if (funk_graph_memory(g, n, 4.0) < 0 || funk_graph_output(g, n) < 0) return 1;
  }
  funk_player *p = funk_player_new();
  funk_player_play(p, g);
  long differ = 0;
  for (int i = 0; i < ZITA_FRAMES; i += 64) {
    float a[64], b[64], *o[2] = {a, b};
    funk_player_render_channels(p, o, 2, 64);
    for (int j = 0; j < 64; j++) differ += a[j] != out[0][i + j] || b[j] != out[1][i + j];
  }
  funk_player_free(p);
  printf("the graph differs from direct calls on %ld samples\n", differ);

  /* a jump in any parameter does not click, and ends on a fresh start's coefficients */
  static const struct { const char *name; int p; float to, limit; } jumps[] = {
      {"predelay", 0, 0.0123f, 8}, {"predelay to 0", 0, 0, 8}, {"crossover", 1, 2000, 1.5f}, {"damping", 2, 500, 2},
      {"lowDecay", 3, 0.3f, 1.5f}, {"midDecay down", 4, 0.2f, 2}, {"midDecay up", 4, 20, 1.5f},
  };
  float *mem = calloc(4 * ZITA_RATE + 2, sizeof *mem), *mem2 = calloc(4 * ZITA_RATE + 2, sizeof *mem2);
  for (size_t j = 0; j < sizeof jumps / sizeof *jumps; j++) {
    static double a[145], b[145];
    double before, after = jump(a, mem, jumps[j].p, jumps[j].to, &before);
    /* a fresh node at the target: the same run, with the parameter there from its start */
    float *m = mem2;
    long long frames2 = 4 * ZITA_RATE + 2;
    memset(b, 0, sizeof b);
    memset(m, 0, (size_t)frames2 * sizeof *m);
    memcpy(&b[0], &m, sizeof m);
    memcpy(&b[1], &frames2, sizeof frames2);
    static float pv[6][64], z[64], o2[64];
    float v[6] = {0.06f, 200, 6000, 3, 2, 1};
    v[jumps[j].p] = jumps[j].to;
    for (int k = 0; k < 6; k++)
      for (int i = 0; i < 64; i++) pv[k][i] = v[k];
    float *in2[8] = {z, z, pv[0], pv[1], pv[2], pv[3], pv[4], pv[5]};
    funk_context cx2 = {ZITA_RATE};
    zita(b, in2, o2, 64, &cx2);
    printf("%s: %s, %s\n", jumps[j].name, after <= jumps[j].limit * before ? "no click" : "CLICK", settled(a, b) ? "settled" : "NOT SETTLED");
  }
  free(mem);
  free(mem2);
  return 0;
}
