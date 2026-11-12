/* projects/funkgraph/events.c over lib/kernels.fk: an audio thread renders
   in calls of odd sizes while this thread sends events and replaces 400
   template sets, each set's pool and graphs freed once its events end.
   Run under AddressSanitizer and ThreadSanitizer too. Each event is a
   tone through dsp_gate, 40 frames at 8000 Hz, so it ends; half also run
   a module that halves it. */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "graph.h"

enum { SETS = 400, EVENTS_PER_SET = 8 };

static funk_kernel gate, mul;
static funk_player *player;
static atomic_int stop;
static atomic_long blocks;
static float peak;

static int control(funk_graph *g, const char *name, float v) {
  int i = funk_graph_control(g, v);
  funk_graph_name(g, i, name);
  return i;
}

/* "s.tone": x through dsp_gate for sustain seconds, then a 1 ms fade */
static funk_graph *tone(double sustain) {
  funk_graph *g = funk_graph_new(64, 8000.0);
  int in[8] = {control(g, "x", 1),        control(g, "sustain", (float)sustain), control(g, "fadeInTime", 0),
               control(g, "fadeTime", 0.001f), control(g, "amp", 1), control(g, "gain", 1), control(g, "overgain", 0),
               control(g, "stop", 0)};
  double st[23] = {0}; /* Gate in lib/kernels.fk */
  funk_graph_add(g, gate, st, sizeof st, in, 8);
  return g;
}

/* "m.half": the chain times 0.5, on both channels */
static funk_graph *half(void) {
  funk_graph *g = funk_graph_new(64, 8000.0);
  int l[2] = {funk_graph_in(g, 0), control(g, "amount", 0.5f)};
  int r[2] = {funk_graph_in(g, 1), l[1]};
  funk_graph_output(g, funk_graph_add(g, mul, NULL, 0, l, 2));
  funk_graph_output(g, funk_graph_add(g, mul, NULL, 0, r, 2));
  return g;
}

/* "m.big": the chain times 1, with 3 MB of memory it does not read, so
   its size class has 2 slots of 8 MB's worth */
static funk_graph *big(void) {
  funk_graph *g = funk_graph_new(64, 8000.0);
  int in[2] = {funk_graph_in(g, 0), control(g, "one", 1)};
  double st[2] = {0};
  int m = funk_graph_add(g, mul, st, sizeof st, in, 2);
  funk_graph_memory(g, m, 3.0 * 1024 * 1024 / 4 / 8000.0);
  funk_graph_output(g, m);
  return g;
}

/* "o.boost": an orbit's effect that adds the dry bus times its parameter k */
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

static funk_templates *boosted(float k) {
  funk_templates *t = funk_templates_new(32);
  if (funk_templates_add(t, "s.tone", tone(0.004)) || funk_templates_add(t, "o.boost", boost(k))) return NULL;
  return t;
}

/* the peak of an event of the tone, with k if k is not negative */
static float boost_peak(float k) {
  const char *n[1] = {"k"};
  float v[1] = {k}, l[64], r[64], *lr[2] = {l, r}, peak = 0;
  funk_player_event(player, funk_player_frames(player) + 64, "s.tone", k < 0 ? 0 : 1, n, v);
  for (int b = 0; b < 3; b++) {
    funk_player_render_channels(player, lr, 2, 64);
    for (int i = 0; i < 64; i++) peak = fmaxf(peak, fabsf(l[i]));
  }
  return peak;
}

static funk_templates *set(double sustain) {
  funk_templates *t = funk_templates_new(32);
  if (funk_templates_add(t, "s.tone", tone(sustain)) || funk_templates_add(t, "m.half", half())) return NULL;
  return t;
}

static void *audio(void *arg) {
  (void)arg;
  float l[97], r[97], *out[2] = {l, r};
  for (int k = 0; !atomic_load(&stop); k++) {
    int n = 17 + k % 81; /* odd sizes: blocks of 64 straddle calls */
    funk_player_render_channels(player, out, 2, n);
    for (int i = 0; i < n; i++) peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(r[i])));
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
  gate = (funk_kernel)dlsym(lib, "dsp_gate");
  mul = (funk_kernel)dlsym(lib, "dsp_mul");
  if (!gate || !mul || !(player = funk_player_new())) return 1;
  /* a template is refused with a bus, or a name taken */
  funk_templates *t = funk_templates_new(4);
  funk_graph *b = funk_graph_new(64, 8000.0);
  funk_graph_bus(b, 0, 0);
  printf("bus refused: %d\n", funk_templates_add(t, "s.bus", b) < 0);
  funk_templates_add(t, "s.tone", tone(0.004));
  printf("name taken: %d\n", funk_templates_add(t, "s.tone", tone(0.004)) < 0);
  funk_templates_free(t);

  /* size classes: 4 events at once with m.big, on this thread; its class
     has 2 slots, so 2 are dropped, and the tones' class still has room */
  t = funk_templates_new(32);
  if (funk_templates_add(t, "s.tone", tone(0.004)) || funk_templates_add(t, "m.big", big()) || funk_player_templates(player, t)) return 1;
  const char *bn[1] = {"~big"};
  for (int e = 0; e < 8; e++) {
    float v[1] = {(float)(e < 4)};
    funk_player_event(player, funk_player_frames(player) + 10, "s.tone", 1, bn, v);
  }
  float l[64], r[64], *lr[2] = {l, r};
  funk_player_render_channels(player, lr, 2, 64);
  int playing;
  uint64_t dropped0, late0;
  funk_player_event_counts(player, &playing, &dropped0, &late0);
  printf("classes: playing %d, dropped %llu\n", playing, (unsigned long long)dropped0);
  for (int i = 0; i < 4; i++) funk_player_render_channels(player, lr, 2, 64);
  funk_player_event_counts(player, &playing, &dropped0, &late0);
  printf("classes, ended: playing %d\n", playing);
  funk_player_templates(player, NULL);
  funk_player_reclaim(player);

  /* an orbit's effect, which no kernel pauses, so it runs from its first
     block: an event sets k, and one without k after one with sets it to
     0; an identical set keeps the effect, a changed one starts it anew */
  if (funk_player_templates(player, boosted(1))) return 1;
  printf("an effect, k 1: %g\n", boost_peak(-1));
  printf("k 0.5: %g\n", boost_peak(0.5f));
  printf("no k after k, so 0: %g\n", boost_peak(-1));
  if (funk_player_templates(player, boosted(1))) return 1;
  printf("an identical set keeps it: %g\n", boost_peak(-1));
  if (funk_player_templates(player, boosted(3))) return 1;
  printf("a changed one is new, k 3: %g\n", boost_peak(-1));
  funk_player_templates(player, NULL);
  funk_player_reclaim(player);

  pthread_t th;
  if (pthread_create(&th, NULL, audio, NULL)) return 1;
  const char *names[2] = {"~half", "orbit"};
  int sent = 0;
  for (int s = 0; s < SETS; s++) {
    if (funk_player_templates(player, set(0.004 + 0.001 * (s % 3)))) return 1;
    int64_t at = funk_player_frames(player);
    for (int e = 0; e < EVENTS_PER_SET; e++) {
      float values[2] = {(float)(e % 2), (float)(e % 3)};
      if (funk_player_event(player, funk_player_frames(player) + 200 + e, "s.tone", 2, names, values)) sent++;
    }
    /* past 64 frames, so the next set's events, 200 on, start after these
       end, at most 263 on: 8 overlap, as the peak's bound counts. These
       may still be due when their set is replaced. */
    long seen = atomic_load(&blocks);
    while (atomic_load(&blocks) < seen + 3 || funk_player_frames(player) < at + 64) {}
    funk_player_reclaim(player);
  }
  /* every event ends: 40 to 56 frames each */
  int64_t until = funk_player_frames(player) + 4000;
  while (funk_player_frames(player) < until) {}
  atomic_store(&stop, 1);
  pthread_join(th, NULL);
  int live;
  uint64_t dropped, late;
  funk_player_event_counts(player, &live, &dropped, &late);
  printf("sent %d, playing %d, dropped %llu\n", sent, live, (unsigned long long)(dropped - 2));
  printf("peak above 1, at most 6: %d\n", peak > 1.0f && peak <= 6.0f); /* 8 overlap, half halved */
  funk_player_templates(player, NULL);
  funk_player_reclaim(player);
  funk_player_free(player);
  return 0;
}
