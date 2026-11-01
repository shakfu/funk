/* lib/kernels.fk's dsp_zita against Faust, at mix 1. zita.expected holds Faust 2.88's
   re.zita_rev1_stereo(60, 200, 6000, 3, 2, 48000), compiled with -double and run
   by its interpreter, on zita_stimulus.h: every 4000th frame, 7 decimals. The
   kernels print the same. Then the same kernels as two nodes of a graph, which is
   how a patch plays them: both channels must equal a direct call, sample by sample. */
#include <dlfcn.h>
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

int main(int argc, char **argv) {
  void *lib = argc > 1 ? dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) : NULL;
  zita = lib ? (funk_kernel)dlsym(lib, "dsp_zita") : NULL;
  if (!zita) return 1;
  zita_stimulus(l, r);
  /* direct calls */
  static double st[2][141];
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
    double zero[141] = {0};
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
  return 0;
}
