/* voices.c -- notes on d.poly's voices; see graph.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "graph.h"

enum { MAX_VOICES = 64 };

typedef struct {
  int64_t on, off; /* frames; off is INT64_MAX until a note-off */
  int note;
  bool held; /* the off is not yet sent */
  char gate[16], freq[16];
} Voice;

struct funk_voices {
  funk_player *p;
  bool (*send)(funk_player *, int64_t, const char *, float);
  int n;
  int64_t sent; /* the last event's frame: sends stay in time order */
  Voice v[];
};

funk_voices *funk_voices_new(funk_player *p, int n, bool audio_thread) {
  if (n < 1 || n > MAX_VOICES) return NULL;
  funk_voices *vs = calloc(1, sizeof *vs + (size_t)n * sizeof(Voice));
  if (!vs) return NULL;
  vs->p = p;
  vs->send = audio_thread ? funk_player_send_audio : funk_player_send;
  vs->n = n;
  vs->sent = INT64_MIN;
  for (int i = 0; i < n; i++) {
    vs->v[i].off = INT64_MIN;
    snprintf(vs->v[i].gate, sizeof vs->v[i].gate, "v%d.gate", i + 1);
    snprintf(vs->v[i].freq, sizeof vs->v[i].freq, "v%d.freq", i + 1);
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
    for (int i = 0; i < vs->n; i++)
      if (vs->v[i].held && vs->v[i].off < f && (!v || vs->v[i].off < v->off)) v = &vs->v[i];
    if (!v) return ok;
    ok &= release(vs, v, v->off);
  }
}

bool funk_voices_on(funk_voices *vs, int64_t s, int64_t e, int note, int vel) {
  bool ok = funk_voices_until(vs, s + 1);
  if (s < vs->sent) s = vs->sent;
  Voice *v = &vs->v[0];
  for (int i = 1; i < vs->n; i++) {
    const Voice *w = &vs->v[i];
    bool fi = !w->held, fv = !v->held;
    if (fi != fv ? fi : fi ? w->off < v->off : w->on < v->on) v = &vs->v[i];
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
  v->note = note;
  v->held = true;
  return ok;
}

bool funk_voices_off(funk_voices *vs, int64_t frame, int note) {
  Voice *v = NULL;
  for (int i = 0; i < vs->n; i++)
    if (vs->v[i].held && vs->v[i].note == note && (!v || vs->v[i].on < v->on)) v = &vs->v[i];
  if (!v) return true;
  bool ok = funk_voices_until(vs, frame);
  if (!v->held) return ok; /* its own off came first */
  v->off = frame;
  return ok & release(vs, v, frame);
}

bool funk_voices_hush(funk_voices *vs, int64_t frame) {
  bool ok = true;
  for (int i = 0; i < vs->n; i++)
    if (vs->v[i].held) ok &= release(vs, &vs->v[i], frame);
  return ok;
}
