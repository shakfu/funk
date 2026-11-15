/* projects/funkgraph's audio-thread API allocates, frees and locks nothing:
   run with tests/noalloc.c inserted (check.sh's $NOALLOC_LIB), which counts
   such calls on the audio thread. It renders in odd sizes, and gives voices
   notes, sample triggers with cut groups, and template events with a module
   and an orbit's effect, while this thread replaces graphs and template sets. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "graph.h"

enum { BLOCKS = 4000, SETS = 100 };

static funk_kernel gate, mul;
static funk_player *player;
static funk_voices *voices;
static atomic_int done;
static atomic_long blocks;
static void (*arm)(int);

static int control(funk_graph *g, const char *name, float v) {
  int i = funk_graph_control(g, v);
  funk_graph_name(g, i, name);
  return i;
}

/* the voices' and a sample's controls, summed through mul */
static funk_graph *played(float k) {
  static const char *const names[] = {"v1.gate",       "v1.freq",       "v2.gate",       "v2.freq",       "k60.gate",
                                      "s.bd:0.1.gate", "s.bd:0.1.gain", "s.bd:0.1.stop", "s.bd:0.2.gate", "s.bd:0.2.gain",
                                      "s.bd:0.2.stop", "c1.v1.gate",    "c1.v1.freq"};
  funk_graph *g = funk_graph_new(64, 8000.0);
  int sum = control(g, "k", k);
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    int in[2] = {sum, control(g, names[i], 0)};
    sum = funk_graph_add(g, mul, NULL, 0, in, 2);
  }
  funk_graph_output(g, sum);
  return g;
}

static funk_graph *tone(double sustain) {
  funk_graph *g = funk_graph_new(64, 8000.0);
  int in[8] = {control(g, "x", 1),        control(g, "sustain", (float)sustain), control(g, "fadeInTime", 0),
               control(g, "fadeTime", 0.001f), control(g, "amp", 1), control(g, "gain", 1), control(g, "overgain", 0),
               control(g, "stop", 0)};
  double st[23] = {0}; /* Gate in lib/kernels.fk */
  funk_graph_add(g, gate, st, sizeof st, in, 8);
  return g;
}

static funk_graph *half(void) {
  funk_graph *g = funk_graph_new(64, 8000.0);
  int l[2] = {funk_graph_in(g, 0), control(g, "amount", 0.5f)};
  int r[2] = {funk_graph_in(g, 1), l[1]};
  funk_graph_output(g, funk_graph_add(g, mul, NULL, 0, l, 2));
  funk_graph_output(g, funk_graph_add(g, mul, NULL, 0, r, 2));
  return g;
}

static funk_graph *boost(float k) {
  funk_graph *g = funk_graph_new(64, 8000.0);
  int k_ = control(g, "k", k);
  int l[2] = {funk_graph_in(g, 0), k_}, r[2] = {funk_graph_in(g, 1), k_};
  funk_graph_output(g, l[0]);
  funk_graph_output(g, r[0]);
  funk_graph_output(g, funk_graph_add(g, mul, NULL, 0, l, 2));
  funk_graph_output(g, funk_graph_add(g, mul, NULL, 0, r, 2));
  return g;
}

static funk_templates *set(int s) {
  funk_templates *t = funk_templates_new(32);
  if (funk_templates_add(t, "s.tone", tone(0.004 + 0.001 * (s % 3))) || funk_templates_add(t, "m.half", half()) ||
      funk_templates_add(t, "o.boost", boost(s % 2 ? 1.0f : 0.5f)))
    return NULL;
  return t;
}

static void *audio(void *arg) {
  (void)arg;
  static const char *const tn[3] = {"~half", "orbit", "gain"}, *const sn[2] = {"cut", "gain"}, *const vn[1] = {"gain"};
  float l[97], r[97], *out[2] = {l, r};
  arm(1);
  for (int k = 0; k < BLOCKS; k++) {
    int n = 17 + k % 81; /* odd sizes: blocks of 64 straddle calls */
    int64_t f = funk_player_frames(player);
    float tv[3] = {(float)(k % 2), (float)(k % 3), 0.5f}, sv[2] = {(float)(k % 3) - 1, 0.8f}, vv[1] = {0.7f};
    funk_voices_trigger(voices, f + 1, "s.tone", 3, tn, tv);
    funk_voices_trigger(voices, f + 2, "s.bd:0", 2, sn, sv);
    if (k % 4 == 0) funk_voices_on_with(voices, f + 3, f + 40, k % 2, 60, 100, 1, vn, vv);
    if (k % 4 == 2) funk_voices_off(voices, f + 3, 0, 60);
    funk_player_render_channels(player, out, 2, n);
    atomic_fetch_add(&blocks, 1);
  }
  arm(0);
  atomic_store(&done, 1);
  return NULL;
}

int main(int argc, char **argv) {
  void *lib = argc > 1 ? dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) : NULL;
  int (*hits)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "noalloc_hits");
  arm = (void (*)(int))dlsym(RTLD_DEFAULT, "noalloc_arm");
  if (!lib || !arm || !hits) {
    fprintf(stderr, "needs the kernels and tests/noalloc.c inserted: %s\n", lib ? "no noalloc_arm" : dlerror());
    return 1;
  }
  gate = (funk_kernel)dlsym(lib, "dsp_gate");
  mul = (funk_kernel)dlsym(lib, "dsp_mul");
  if (!gate || !mul || !(player = funk_player_new()) || !(voices = funk_voices_new(player, 2, true))) return 1;
  funk_player_play(player, played(1));
  if (funk_player_templates(player, set(0))) return 1;
  pthread_t th;
  if (pthread_create(&th, NULL, audio, NULL)) return 1;
  int most = 0;
  for (int s = 1; !atomic_load(&done); s++) {
    long seen = atomic_load(&blocks);
    while (!atomic_load(&done) && atomic_load(&blocks) < seen + BLOCKS / SETS) {
      int live;
      funk_player_event_counts(player, &live, NULL, NULL);
      if (live > most) most = live;
    }
    if (funk_player_templates(player, set(s))) return 1;
    if (s % 2) funk_player_play(player, played((float)s));
    funk_player_reclaim(player);
  }
  pthread_join(th, NULL);
  printf("events played: %d\n", most > 0);
  printf("hits on the audio thread: %d\n", hits());
  funk_player_templates(player, NULL);
  funk_player_play(player, NULL);
  funk_player_reclaim(player);
  funk_voices_free(voices);
  funk_player_free(player);
  return 0;
}
