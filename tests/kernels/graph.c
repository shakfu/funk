/* hosts/graph/graph.c over the kernels of dsp.fk. First host.c's chain as
   a graph, in blocks of 8 split into calls of at most 5 frames: the output
   must equal host.c's clip rows. Then an audio thread renders while this
   thread plays and reclaims 2000 graphs; run under ThreadSanitizer too. */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "graph.h"

enum { N = 8, SWAPS = 2000 };

static funk_kernel saw, sine, pole, mul, clip, voice;
static funk_player *player;
static atomic_int stop, blocks;
static int bad;

static funk_kernel get(void *lib, const char *name) {
  funk_kernel k = (funk_kernel)dlsym(lib, name);
  if (!k) fprintf(stderr, "no %s\n", name);
  return k;
}

/* host.c's chain: saw into a one-pole filter, times a sine, clipped */
static funk_graph *chain(int max_block) {
  funk_graph *g = funk_graph_new(max_block);
  double osc1[2] = {0.0, 1000.0}, osc2[2] = {0.25, 500.0}, lp[2] = {0.0, 0.5};
  int a = funk_graph_add(g, saw, osc1, sizeof osc1, NULL, 0);
  int b = funk_graph_add(g, pole, lp, sizeof lp, &a, 1);
  int c = funk_graph_add(g, sine, osc2, sizeof osc2, NULL, 0);
  int bc[2] = {b, c};
  int d = funk_graph_add(g, mul, NULL, 0, bc, 2);
  funk_graph_add(g, clip, NULL, 0, &d, 1);
  return g;
}

static void *audio(void *arg) {
  (void)arg;
  float out[64];
  while (!atomic_load(&stop)) {
    funk_player_render(player, out, 64);
    for (int i = 0; i < 64; i++) bad += !(fabsf(out[i]) <= 1.0f);
    atomic_fetch_add(&blocks, 1);
  }
  return NULL;
}

int main(int argc, char **argv) {
  void *lib = argc > 1 ? dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) : NULL;
  if (!lib) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  saw = get(lib, "k_saw"), sine = get(lib, "k_sine"), pole = get(lib, "k_onepole");
  mul = get(lib, "k_mul"), clip = get(lib, "k_clip"), voice = get(lib, "k_voice");
  if (!saw || !sine || !pole || !mul || !clip || !voice) return 1;

  player = funk_player_new();
  float out[N];
  funk_player_render(player, out, N);
  printf("silent  %9.6f\n", out[0]);
  int bad_input = 7;
  funk_graph *g = funk_graph_new(4);
  printf("forward input rejected: %s\n", funk_graph_add(g, saw, NULL, 0, &bad_input, 1) < 0 ? "yes" : "no");
  funk_graph_free(g);
  funk_player_play(player, chain(5));
  for (int block = 0; block < 2; block++) {
    funk_player_render(player, out, N);
    printf("clip    ");
    for (int i = 0; i < N; i++) printf(" %9.6f", out[i]);
    printf("\n");
  }

  pthread_t t;
  pthread_create(&t, NULL, audio, NULL);
  for (int i = 0; i < SWAPS; i++) {
    funk_graph *h;
    if (i % 2) {
      double v[4] = {0.0, 100.0 + i, 0.0, 0.5};
      h = funk_graph_new(64);
      funk_graph_add(h, voice, v, sizeof v, NULL, 0);
    } else {
      funk_player_play(player, NULL); /* silence between, too */
      h = chain(64);
    }
    funk_player_play(player, h);
    for (int b = atomic_load(&blocks); i % 10 == 0 && atomic_load(&blocks) < b + 2;) /* let it render this graph */
      ;
  }
  atomic_store(&stop, 1);
  pthread_join(t, NULL);
  funk_player_reclaim(player);
  funk_player_free(player);
  printf("%d swaps, samples outside [-1, 1]: %d, blocks rendered: at least %s\n", SWAPS, bad, atomic_load(&blocks) >= SWAPS / 5 ? "400" : "fewer");
  return 0;
}
