/* voices.c -- notes on d.poly's voices; see graph.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "graph.h"

enum { MAX_VOICES = 64, GROUPS = 17 }; /* group 0 is shared, group C is channel C */

typedef struct {
  int64_t on, off; /* frames; off is INT64_MAX until a note-off */
  int chan, note;
  bool held; /* the off is not yet sent */
  char gate[16], freq[16];
} Voice;

struct funk_voices {
  funk_player *p;
  bool (*send)(funk_player *, int64_t, const char *, float);
  int n;       /* voices per group */
  int64_t sent; /* the last event's frame: sends stay in time order */
  Voice v[];   /* group g's are v[g * n] to v[g * n + n - 1] */
};

funk_voices *funk_voices_new(funk_player *p, int n, bool audio_thread) {
  if (n < 1 || n > MAX_VOICES) return NULL;
  funk_voices *vs = calloc(1, sizeof *vs + (size_t)(GROUPS * n) * sizeof(Voice));
  if (!vs) return NULL;
  vs->p = p;
  vs->send = audio_thread ? funk_player_send_audio : funk_player_send;
  vs->n = n;
  vs->sent = INT64_MIN;
  for (int g = 0; g < GROUPS; g++)
    for (int i = 0; i < n; i++) {
      Voice *v = &vs->v[g * n + i];
      v->off = INT64_MIN;
      if (g) {
        snprintf(v->gate, sizeof v->gate, "c%d.v%d.gate", g, i + 1);
        snprintf(v->freq, sizeof v->freq, "c%d.v%d.freq", g, i + 1);
      } else {
        snprintf(v->gate, sizeof v->gate, "v%d.gate", i + 1);
        snprintf(v->freq, sizeof v->freq, "v%d.freq", i + 1);
      }
    }
  return vs;
}

void funk_voices_free(funk_voices *vs) { free(vs); }

static bool send(funk_voices *vs, int64_t frame, const char *name, float x) {
  if (frame < vs->sent) frame = vs->sent;
  vs->sent = frame;
  return vs->send(vs->p, frame, name, x);
}

static bool release(funk_voices *vs, Voice *v, int64_t frame) {
  v->held = false;
  return send(vs, frame, v->gate, 0);
}

bool funk_voices_until(funk_voices *vs, int64_t f) {
  bool ok = true;
  for (;;) {
    Voice *v = NULL;
    for (int i = 0; i < GROUPS * vs->n; i++)
      if (vs->v[i].held && vs->v[i].off < f && (!v || vs->v[i].off < v->off)) v = &vs->v[i];
    if (!v) return ok;
    ok &= release(vs, v, v->off);
  }
}

bool funk_voices_on(funk_voices *vs, int64_t s, int64_t e, int chan, int note, int vel) {
  bool ok = funk_voices_until(vs, s + 1);
  if (s < vs->sent) s = vs->sent;
  chan &= 15;
  Voice *g = &vs->v[(chan + 1) * vs->n];
  if (!funk_player_has(vs->p, g->gate)) g = &vs->v[0];
  Voice *v = g;
  for (int i = 1; i < vs->n; i++) {
    const Voice *w = &g[i];
    bool fi = !w->held, fv = !v->held;
    if (fi != fv ? fi : fi ? w->off < v->off : w->on < v->on) v = &g[i];
  }
  int64_t at = s;
  if (v->held || v->off >= s) { /* a gate must rise to restart an envelope */
    ok &= send(vs, s, v->gate, 0);
    at = s + 1;
  }
  ok &= send(vs, s, v->freq, (float)(440 * pow(2, (note - 69) / 12.0)));
  ok &= send(vs, at, v->gate, vel / 127.0f);
  v->on = s;
  v->off = e > at ? e : at + 1;
  v->chan = chan;
  v->note = note;
  v->held = true;
  return ok;
}

/* Every group: the graph may have changed since the note-on. */
bool funk_voices_off(funk_voices *vs, int64_t frame, int chan, int note) {
  Voice *v = NULL;
  chan &= 15;
  for (int i = 0; i < GROUPS * vs->n; i++) {
    const Voice *w = &vs->v[i];
    if (w->held && w->chan == chan && w->note == note && (!v || w->on < v->on)) v = &vs->v[i];
  }
  if (!v) return true;
  bool ok = funk_voices_until(vs, frame);
  if (!v->held) return ok; /* its own off came first */
  v->off = frame;
  return ok & release(vs, v, frame);
}

bool funk_voices_hush(funk_voices *vs, int64_t frame) {
  bool ok = true;
  for (int i = 0; i < GROUPS * vs->n; i++)
    if (vs->v[i].held) ok &= release(vs, &vs->v[i], frame);
  return ok;
}
