/* voices.c -- notes on d.poly's voices; see graph.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graph.h"

enum { MAX_VOICES = 64, GROUPS = 17 }; /* group 0 is shared, group C is channel C */

typedef struct {
  int64_t on, off; /* frames; off is INT64_MAX until a note-off */
  int chan, note;
  bool held;  /* the off is not yet sent */
  bool keyed; /* counted in its note's key */
  char gate[16], freq[16];
} Voice;

enum { KEYS = 128 };

typedef struct {
  int held;    /* voices of this note that are held and keyed */
  int64_t off; /* when the key's gate last fell */
  char gate[12];
} Key;

/* A sample name's voices, as d.kit gives them: the next to take, and the cut
   group and orbit each last played in (DirtGateCutGroup in SuperDirt). */
enum { SAMPLE_KEYS = 256, SAMPLE_VOICES = 16, SAMPLE_KEY = 64 };

typedef struct {
  char key[SAMPLE_KEY]; /* "s.bd:3"; empty if unused */
  int next;
  float cut[SAMPLE_VOICES], orbit[SAMPLE_VOICES];
} Sample;

struct funk_voices {
  Sample samples[SAMPLE_KEYS];
  funk_player *p;
  bool (*send)(funk_player *, int64_t, const char *, float);
  int n;       /* voices per group */
  int64_t sent; /* the last event's frame: sends stay in time order */
  Key keys[KEYS];
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
  for (int k = 0; k < KEYS; k++) {
    vs->keys[k].off = INT64_MIN;
    snprintf(vs->keys[k].gate, sizeof vs->keys[k].gate, "k%d.gate", k);
  }
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

/* v's note no longer holds its key; the last one lets it fall */
static bool key_off(funk_voices *vs, Voice *v, int64_t frame) {
  if (!v->keyed) return true;
  v->keyed = false;
  Key *k = &vs->keys[v->note];
  if (--k->held > 0) return true;
  bool ok = send(vs, frame, k->gate, 0);
  k->off = vs->sent;
  return ok;
}

static bool release(funk_voices *vs, Voice *v, int64_t frame) {
  v->held = false;
  bool ok = send(vs, frame, v->gate, 0);
  return ok & key_off(vs, v, frame);
}

/* note's key rises at frame s, or a frame later if it must fall first */
static bool key_on(funk_voices *vs, Voice *v, int64_t s, int vel) {
  Key *k = &vs->keys[v->note];
  if (!funk_player_has(vs->p, k->gate)) return true;
  bool ok = true;
  if (k->held > 0 || k->off >= s) {
    ok &= send(vs, s, k->gate, 0);
    s = vs->sent + 1;
  }
  ok &= send(vs, s, k->gate, vel / 127.0f);
  k->held++;
  v->keyed = true;
  return ok;
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

/* prefix.name, if the graph has that control, to value from frame */
static bool send_named(funk_voices *vs, int64_t frame, const char *prefix, size_t n, const char *name, float value) {
  char c[96];
  if (snprintf(c, sizeof c, "%.*s.%s", (int)n, prefix, name) >= (int)sizeof c || !funk_player_has(vs->p, c)) return true;
  return send(vs, frame, c, value);
}

bool funk_voices_on(funk_voices *vs, int64_t s, int64_t e, int chan, int note, int vel) {
  return funk_voices_on_with(vs, s, e, chan, note, vel, 0, NULL, NULL);
}

bool funk_voices_on_with(funk_voices *vs, int64_t s, int64_t e, int chan, int note, int vel, int n,
                         const char *const *names, const float *values) {
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
  ok &= key_off(vs, v, s); /* a stolen voice's note */
  ok &= send(vs, s, v->freq, (float)(440 * pow(2, (note - 69) / 12.0)));
  for (int i = 0; i < n; i++) ok &= send_named(vs, s, v->gate, strlen(v->gate) - 5, names[i], values[i]); /* "vK.gate" less ".gate" */
  ok &= send(vs, at, v->gate, vel / 127.0f);
  v->on = s;
  v->off = e > at ? e : at + 1;
  v->chan = chan;
  v->note = note & (KEYS - 1);
  v->held = true;
  return ok & key_on(vs, v, at, vel);
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

/* names[i]'s value, or d */
static float named(int n, const char *const *names, const float *values, const char *name, float d) {
  for (int i = 0; i < n; i++)
    if (!strcmp(names[i], name)) return values[i];
  return d;
}

/* prefix's entry, made if new; NULL when the table is full */
static Sample *sample_for(funk_voices *vs, const char *prefix) {
  if (strlen(prefix) >= SAMPLE_KEY) return NULL;
  for (int i = 0; i < SAMPLE_KEYS; i++) {
    Sample *s = &vs->samples[i];
    if (!s->key[0]) {
      snprintf(s->key, sizeof s->key, "%s", prefix);
      return s;
    }
    if (!strcmp(s->key, prefix)) return s;
  }
  return NULL;
}

/* how many voices the graph gives prefix: prefix.1.gate, prefix.2.gate, ... */
static int voices_of(funk_voices *vs, const char *prefix) {
  char c[96];
  int v = 0;
  while (v < SAMPLE_VOICES && snprintf(c, sizeof c, "%s.%d.gate", prefix, v + 1) < (int)sizeof c && funk_player_has(vs->p, c)) v++;
  return v;
}

bool funk_voices_trigger(funk_voices *vs, int64_t frame, const char *prefix, int n, const char *const *names,
                         const float *values) {
  bool ok = funk_voices_until(vs, frame + 1);
  if (frame < vs->sent) frame = vs->sent;
  Sample *s = sample_for(vs, prefix);
  int count = s ? voices_of(vs, prefix) : 0;
  char voice[96];
  if (!count) snprintf(voice, sizeof voice, "%s", prefix); /* a graph without voices */
  else {
    if (s->next >= count) s->next = 0;
    snprintf(voice, sizeof voice, "%s.%d", prefix, s->next + 1);
  }
  /* A cut group, as SuperDirt's: a voice of the same orbit last played with
     this cut's size stops, of any sample for a positive cut, of this one for
     a negative. */
  float cut = named(n, names, values, "cut", 0), orbit = named(n, names, values, "orbit", 0);
  char stops[64][96];
  int nstops = 0;
  if (cut != 0)
    for (int i = 0; i < SAMPLE_KEYS && vs->samples[i].key[0]; i++)
      for (int v = 0; v < SAMPLE_VOICES; v++) {
        Sample *o = &vs->samples[i];
        if (o->cut[v] == 0 || o->cut[v] != fabsf(cut) || o->orbit[v] != orbit || (cut < 0 && o != s) || nstops == 64) continue;
        if (o == s && v == s->next && count) continue; /* the voice this event takes */
        snprintf(stops[nstops], sizeof stops[0], "%s.%d", o->key, v + 1);
        ok &= send_named(vs, frame, stops[nstops], strlen(stops[nstops]), "stop", 1);
        nstops++;
      }
  size_t len = strlen(voice);
  for (int i = 0; i < n; i++) ok &= send_named(vs, frame, voice, len, names[i], values[i]);
  ok &= send_named(vs, frame, voice, len, "gate", 1);
  for (int i = 0; i < nstops; i++) ok &= send_named(vs, frame + 1, stops[i], strlen(stops[i]), "stop", 0);
  ok &= send_named(vs, frame + 1, voice, len, "gate", 0);
  if (count) {
    s->cut[s->next] = fabsf(cut), s->orbit[s->next] = orbit;
    s->next = (s->next + 1) % count;
  }
  return ok;
}

bool funk_voices_hush(funk_voices *vs, int64_t frame) {
  bool ok = true;
  for (int i = 0; i < GROUPS * vs->n; i++)
    if (vs->v[i].held) ok &= release(vs, &vs->v[i], frame);
  return ok;
}
