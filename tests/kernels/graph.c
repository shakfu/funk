/* hosts/graph/graph.c over the kernels of chain.fk. First host.c's chain as
   a graph, in blocks of 8 split into calls of at most 5 frames: the output
   must equal host.c's clip rows. Then an audio thread renders while this
   thread plays and reclaims 2000 graphs; run under ThreadSanitizer too.
   Between, named nodes' states carry across swaps, and controls take
   events at their frames. */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "graph.h"

enum { N = 8, SWAPS = 2000, VOICES = 200 };

static funk_kernel saw, sine, pole, mul, clip, voice;
static funk_player *player;
static atomic_int stop, blocks;
static int bad;

static funk_kernel get(void *lib, const char *name) {
  funk_kernel k = (funk_kernel)dlsym(lib, name);
  if (!k) fprintf(stderr, "no %s\n", name);
  return k;
}

/* host.c's chain: saw into a one-pole filter, times a sine, clipped; its
   stateful nodes named if named, the saw at freq */
static funk_graph *chain(int max_block, int named, double freq) {
  funk_graph *g = funk_graph_new(max_block, 8000.0);
  double osc1[2] = {0.0, freq}, osc2[2] = {0.25, 500.0}, lp[2] = {0.0, 0.5};
  int a = funk_graph_add(g, saw, osc1, sizeof osc1, NULL, 0);
  int b = funk_graph_add(g, pole, lp, sizeof lp, &a, 1);
  int c = funk_graph_add(g, sine, osc2, sizeof osc2, NULL, 0);
  int bc[2] = {b, c};
  int d = funk_graph_add(g, mul, NULL, 0, bc, 2);
  funk_graph_add(g, clip, NULL, 0, &d, 1);
  if (named) funk_graph_name(g, a, "saw"), funk_graph_name(g, b, "lp"), funk_graph_name(g, c, "sine");
  return g;
}

/* one oscillator, named */
static funk_graph *one(funk_kernel k, double freq) {
  funk_graph *g = funk_graph_new(5, 8000.0);
  double osc[2] = {k == sine ? 0.25 : 0.0, freq};
  funk_graph_name(g, funk_graph_add(g, k, osc, sizeof osc, NULL, 0), k == sine ? "sine" : "saw");
  return g;
}

/* a named sine at 500 times a named saw at freq */
static funk_graph *product(double freq) {
  funk_graph *g = funk_graph_new(5, 8000.0);
  double osc1[2] = {0.0, freq}, osc2[2] = {0.25, 500.0};
  int ab[2] = {funk_graph_add(g, sine, osc2, sizeof osc2, NULL, 0), funk_graph_add(g, saw, osc1, sizeof osc1, NULL, 0)};
  funk_graph_name(g, ab[0], "sine");
  funk_graph_name(g, ab[1], "saw");
  funk_graph_add(g, mul, NULL, 0, ab, 2);
  return g;
}

/* a control "gate" starting at value, times a saw named "osc" at 0 Hz,
   which is -1: the output is -gate */
static funk_graph *gated(float value) {
  funk_graph *g = funk_graph_new(8, 8000.0);
  double osc[2] = {0.0, 0.0};
  int ab[2] = {funk_graph_control(g, value), funk_graph_add(g, saw, osc, sizeof osc, NULL, 0)};
  funk_graph_name(g, ab[0], "gate");
  funk_graph_name(g, ab[1], "osc");
  funk_graph_add(g, mul, NULL, 0, ab, 2);
  return g;
}

static void show(funk_player *p, const char *what) {
  float out[N];
  funk_player_render(p, out, N);
  printf("%-7s", what);
  for (int i = 0; i < N; i++) printf(" %6.3f", -out[i]);
  printf("\n");
}

/* n frames of a new player's output for g, after skip frames */
static void fresh(funk_graph *g, int skip, float *out, int n) {
  funk_player *p = funk_player_new();
  funk_player_play(p, g);
  float tmp[2 * N];
  funk_player_render(p, tmp, skip + n);
  for (int i = 0; i < n; i++) out[i] = tmp[skip + i];
  funk_player_free(p);
}

static const char *same(const float *a, const float *b) {
  for (int i = 0; i < N; i++)
    if (a[i] != b[i]) return "no";
  return "yes";
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
  funk_graph *g = funk_graph_new(4, 8000.0);
  printf("forward input rejected: %s\n", funk_graph_add(g, saw, NULL, 0, &bad_input, 1) < 0 ? "yes" : "no");
  funk_graph_free(g);
  funk_player_play(player, chain(5, 0, 1000.0));
  for (int block = 0; block < 2; block++) {
    funk_player_render(player, out, N);
    printf("clip    ");
    for (int i = 0; i < N; i++) printf(" %9.6f", out[i]);
    printf("\n");
  }

  /* named states carry across a swap, even past a graph never rendered */
  float want[N], restart[N];
  fresh(chain(5, 0, 1000.0), N, want, N);
  fresh(chain(5, 0, 1000.0), 0, restart, N);
  funk_player_play(player, chain(5, 1, 1000.0));
  funk_player_render(player, out, N);
  funk_player_play(player, chain(5, 1, 1000.0));
  funk_player_play(player, chain(5, 1, 1000.0));
  funk_player_render(player, out, N);
  printf("named states continue: %s\n", same(out, want));
  funk_player_play(player, chain(5, 0, 1000.0));
  funk_player_render(player, out, N);
  printf("unnamed states restart: %s\n", same(out, restart));
  /* a sine times a saw: a new saw frequency restarts the saw alone */
  float sw[N], sn[N], prod[N];
  funk_player *q = funk_player_new();
  funk_player_play(q, product(1000.0));
  funk_player_render(q, out, N);
  funk_player_play(q, product(900.0));
  funk_player_render(q, out, N);
  funk_player_free(q);
  fresh(one(saw, 900.0), 0, sw, N);
  fresh(one(sine, 500.0), N, sn, N);
  for (int i = 0; i < N; i++) prod[i] = sn[i] * sw[i]; /* as k_mul: a double product of floats is exact */
  printf("changed state restarts, others continue: %s\n", same(out, prod));
  g = funk_graph_new(4, 8000.0);
  int n0 = funk_graph_add(g, saw, NULL, 0, NULL, 0), n1 = funk_graph_add(g, saw, NULL, 0, NULL, 0);
  funk_graph_name(g, n0, "x");
  printf("duplicate name rejected: %s\n", funk_graph_name(g, n1, "x") < 0 ? "yes" : "no");
  funk_graph_free(g);

  /* a control: events from their frame, later ones waiting for their block */
  funk_player *c = funk_player_new();
  funk_player_play(c, gated(0.0f));
  funk_player_send(c, 3, "gate", 1.0f);
  funk_player_send(c, 5, "nothing", 9.0f);
  funk_player_send(c, 6, "osc", 9.0f); /* a kernel's node: ignored */
  funk_player_send(c, 6, "gate", 0.5f);
  funk_player_send(c, 10, "gate", 0.25f);
  show(c, "events");
  show(c, "next");
  funk_player_send(c, 2, "gate", 0.75f); /* late: from the next block */
  show(c, "late");
  funk_player_play(c, gated(0.0f)); /* the same initial value: the value carries */
  show(c, "swap");
  funk_player_play(c, gated(0.125f));
  show(c, "reset");
  printf("frames %lld\n", (long long)funk_player_frames(c));
  funk_player_free(c);

  pthread_t t;
  pthread_create(&t, NULL, audio, NULL);
  for (int i = 0; i < SWAPS; i++) {
    funk_graph *h;
    if (i % 5) {
      double v[4] = {0.0, 100.0 + i % 3, 0.0, 0.5};
      h = funk_graph_new(64, 8000.0);
      int ab[2];
      for (int j = 0; j < VOICES; j++) { /* names enough that a copy takes a while */
        char name[16];
        snprintf(name, sizeof name, "v%d", j);
        funk_graph_name(h, ab[0] = funk_graph_add(h, voice, v, sizeof v, NULL, 0), name);
      }
      funk_graph_name(h, ab[1] = funk_graph_control(h, 1.0f), "gain"); /* which events change as it plays */
      funk_graph_add(h, mul, NULL, 0, ab, 2);
    } else {
      funk_player_play(player, NULL); /* silence between, too */
      h = chain(64, 1, 1000.0);
    }
    funk_player_play(player, h);
    funk_player_send(player, funk_player_frames(player) + i % 64, "gain", i % 2 ? 0.5f : 1.0f);
    for (int b = atomic_load(&blocks); i % 5 == 2 && atomic_load(&blocks) < b + 2;) /* let it copy and render */
      funk_player_reclaim(player);
  }
  atomic_store(&stop, 1);
  pthread_join(t, NULL);
  funk_player_reclaim(player);
  funk_player_free(player);
  printf("%d swaps, samples outside [-1, 1]: %d, blocks rendered: at least %s\n", SWAPS, bad, atomic_load(&blocks) >= SWAPS / 5 ? "400" : "fewer");
  return 0;
}
