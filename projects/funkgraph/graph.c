/* graph.c -- see graph.h. The audio thread loads the current graph at each
   block's start and advances the epoch at its end. A graph replaced when
   the epoch was e may still be in the block running then, which ends by
   e + 1, and the next block copies named states from it, which ends by
   e + 2. So it is freed once the epoch exceeds e + 1. */
#include "graph_internal.h"
#include <math.h>
#include <pthread.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EVENTS = 4096 }; /* a power of two */
enum { MAX_CHUNK = 4096 }; /* frames per render step: the event layer's bus holds twice this */

typedef struct {
  int64_t frame;
  uint64_t key;
  float value;
} Event;

struct funk_player {
  _Atomic(funk_graph *) current;
  _Atomic uint64_t epoch;
  funk_graph *retired;
  funk_graph *last; /* the audio thread's: the graph it rendered last */
  _Atomic int64_t frames;
  _Atomic uint64_t head, tail; /* events sent, and taken by the audio thread */
  Event events[EVENTS];
  uint64_t ahead, atail; /* the audio thread's own queue */
  Event audio[EVENTS];
  funk_events *ev; /* the event layer (events.c) */
};

funk_events *funk_player_events_of(funk_player *p) { return p->ev; }

uint64_t funk_player_epoch(funk_player *p) { return atomic_load(&p->epoch); }

static uint64_t mix(uint64_t h, uint64_t v) {
  h = (h ^ v) * 0x9e3779b97f4a7c15u;
  return h ^ (h >> 32);
}

/* scsynth's gSine and gInvSine (Samp.cpp), 8192 points a cycle: the
   kernels' sineAt and cscAt, which compute them without a table */
enum { SINE = 8193 };
static double tables[2 * SINE];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static void tables_init(void) {
  for (int i = 0; i < SINE; i++) {
    float s = (float)sin(i * (6.283185307179586 / 8192.0));
    tables[i] = s;
    int pole = i <= 8 || i >= 8184 || (i >= 4088 && i <= 4104);
    tables[SINE + i] = pole ? (float)1.0e20 : (float)(1.0 / s);
  }
}

funk_graph *funk_graph_new(int max_block, double rate) {
  if (max_block < 1 || !(rate > 0)) return NULL;
  funk_graph *g = calloc(1, sizeof *g);
  if (!g) return NULL;
  g->max_block = max_block;
  g->cx.rate = rate;
  pthread_once(&tables_once, tables_init);
  g->cx.tables = tables;
  g->silence = calloc((size_t)max_block, sizeof(float));
  if (!g->silence) {
    free(g);
    return NULL;
  }
  return g;
}

int funk_graph_add(funk_graph *g, funk_kernel k, const void *state, size_t size, const int *inputs, int nin) {
  if (!k || nin < 0) return -1;
  for (int i = 0; i < nin; i++)
    if (inputs[i] < -1 || inputs[i] >= g->n) return -1;
  if (g->n == g->cap) {
    int cap = g->cap ? 2 * g->cap : 8;
    Node *a = realloc(g->nodes, (size_t)cap * sizeof *a);
    if (!a) return -1;
    g->nodes = a;
    g->cap = cap;
  }
  Node x = {k, NULL, NULL, nin, NULL, 0, mix(1, (uint64_t)(uintptr_t)k), NULL, size, NULL, 0, NULL, KERNEL, 0, NULL, 0, 0, 0};
  x.state = size ? calloc(1, size) : NULL;
  x.in = calloc((size_t)(nin ? nin : 1), sizeof *x.in);
  x.out = calloc((size_t)g->max_block, sizeof(float));
  x.ix = malloc((size_t)(nin ? nin : 1) * sizeof *x.ix);
  if ((size && !x.state) || !x.in || !x.out || !x.ix) {
    free(x.state);
    free(x.in);
    free(x.out);
    free(x.ix);
    return -1;
  }
  if (nin) memcpy(x.ix, inputs, (size_t)nin * sizeof *x.ix);
  if (state && size) memcpy(x.state, state, size);
  for (int i = 0; i < nin; i++) {
    const Node *y = inputs[i] < 0 ? NULL : &g->nodes[inputs[i]];
    x.in[i] = y ? y->out : g->silence;
    x.shape = mix(x.shape, !y ? 2 : y->key ? y->key : y->shape); /* a named input by its name */
  }
  g->nodes[g->n] = x;
  return g->n++;
}

uint64_t funk_hash(const char *s) {
  uint64_t h = 14695981039346656037u;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211u;
  return h ? h : 1;
}

/* the first of g's first n names whose key is at least key */
static int lower_n(const funk_graph *g, int n, uint64_t key) {
  int lo = 0, hi = n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (g->names[mid].key < key) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

static int lower(const funk_graph *g, uint64_t key) { return lower_n(g, g->nnames, key); }

int funk_graph_ugen(funk_graph *g, int node) {
  if (node < 0 || node >= g->n || g->nodes[node].kind != KERNEL) return -1;
  g->nodes[node].ugen = 1;
  return 0;
}

int funk_graph_name(funk_graph *g, int node, const char *name) {
  if (node < 0 || node >= g->n || g->nodes[node].key || !name) return -1;
  Node *x = &g->nodes[node];
  uint64_t key = funk_hash(name);
  int at = lower(g, key);
  if (at < g->nnames && g->names[at].key == key) return -1;
  Name *a = realloc(g->names, (size_t)(g->nnames + 1) * sizeof *a);
  if (!a) return -1;
  g->names = a;
  if (x->size && !(x->init = malloc(x->size))) return -1;
  if (x->size) memcpy(x->init, x->state, x->size);
  memmove(a + at + 1, a + at, (size_t)(g->nnames - at) * sizeof *a);
  a[at] = (Name){key, node};
  g->nnames++;
  x->key = key;
  x->exp = name[0] == '>' ? funk_hash(name + 1) : 0;
  if (!strncmp(name, "fed:", 4)) {
    char feed[256];
    snprintf(feed, sizeof feed, "feed:%s", name + 4);
    x->fed = funk_hash(feed);
  }
  return 0;
}

static int by_shape(const void *a, const void *b) {
  const Name *x = a, *y = b;
  if (x->key != y->key) return x->key < y->key ? -1 : 1;
  return x->node - y->node;
}

static int by_key(const void *a, const void *b) {
  const Name *x = a, *y = b;
  return x->key < y->key ? -1 : x->key > y->key;
}

/* Names g's unnamed kernel nodes by shape: the kth of a shape, in the
   order added, by its shape and k. Without memory, they stay unnamed. */
static void name_shapes(funk_graph *g) {
  int n = 0;
  for (int i = 0; i < g->n; i++) n += g->nodes[i].k && !g->nodes[i].key;
  if (!n) return;
  Name *a = realloc(g->names, (size_t)(g->nnames + n) * sizeof *a), *u = malloc((size_t)n * sizeof *u);
  if (a) g->names = a;
  if (!a || !u) {
    free(u);
    return;
  }
  n = 0;
  for (int i = 0; i < g->n; i++)
    if (g->nodes[i].k && !g->nodes[i].key) u[n++] = (Name){g->nodes[i].shape, i};
  qsort(u, (size_t)n, sizeof *u, by_shape);
  int named = g->nnames;
  for (int i = 0, k = 0; i < n; i++) {
    k = i && u[i].key == u[i - 1].key ? k + 1 : 0;
    Node *x = &g->nodes[u[i].node];
    uint64_t key = mix(u[i].key, (uint64_t)k + 1);
    key = key ? key : 1;
    int at = lower_n(g, named, key); /* the given names, still sorted */
    if ((at < named && g->names[at].key == key) || (x->size && !(x->init = malloc(x->size)))) continue;
    if (x->size) memcpy(x->init, x->state, x->size);
    x->key = key;
    g->names[g->nnames++] = (Name){key, u[i].node};
  }
  free(u);
  qsort(g->names, (size_t)g->nnames, sizeof *g->names, by_key);
}

void funk_table_state(Node *x) {
  const float *d = funk_table_data(x->table);
  int64_t frames = funk_table_frames(x->table);
  double rate = funk_table_rate(x->table);
  memcpy(x->state, &d, sizeof d);
  memcpy((char *)x->state + 8, &frames, sizeof frames);
  memcpy((char *)x->state + 16, &rate, sizeof rate);
}

/* The audio thread, at g's first block: the states of old's nodes with a
   name, kernel and initial state that g's have. */
static void carry(funk_graph *g, funk_graph *old) {
  for (int i = 0; i < g->nnames; i++) {
    int at = lower(old, g->names[i].key);
    if (at == old->nnames || old->names[at].key != g->names[i].key) continue;
    Node *x = &g->nodes[g->names[i].node];
    Node *y = &old->nodes[old->names[at].node];
    if (x->k != y->k || x->size != y->size || x->frames != y->frames || (x->size && memcmp(x->init, y->init, x->size))) continue;
    if (x->size) memcpy(x->state, y->state, x->size);
    if (x->mem) { /* by pointer: copying a second of delay would take a block's time. old frees x's. */
      float *m = x->mem;
      x->mem = y->mem;
      y->mem = m;
      memcpy(x->state, &x->mem, sizeof x->mem);
    }
    if (x->table) funk_table_state(x); /* its own data: only the position carries */
  }
}

int funk_graph_memory(funk_graph *g, int node, double seconds) {
  if (node < 0 || node >= g->n || !(seconds >= 0)) return -1;
  Node *x = &g->nodes[node];
  double whole = seconds * g->cx.rate;
  if (!x->k || x->mem || x->size < 2 * sizeof(void *) || whole > INT32_MAX) return -1;
  int64_t frames = (int64_t)whole;
  frames += (frames < whole) + 2;
  x->mem = malloc((size_t)frames * sizeof(float));
  if (!x->mem) return -1;
  memset(x->mem, 0, (size_t)frames * sizeof(float)); /* over calloc: the audio thread must not fault its pages in */
  x->frames = frames;
  return 0;
}

static int give_table(funk_graph *g, int node, funk_table *t) {
  if (!t) return -1;
  g->nodes[node].table = t;
  return 0;
}

static bool takes_table(const funk_graph *g, int node) {
  if (node < 0 || node >= g->n) return false;
  const Node *x = &g->nodes[node];
  return x->k && !x->table && !x->mem && x->size >= 24;
}

int funk_graph_table_file(funk_graph *g, int node, const char *path, int channel) {
  return takes_table(g, node) ? give_table(g, node, funk_table_file(path, channel)) : -1;
}

int funk_graph_table_values(funk_graph *g, int node, const double *values, int64_t frames) {
  return takes_table(g, node) ? give_table(g, node, funk_table_values(values, frames)) : -1;
}

int funk_graph_control(funk_graph *g, float value) {
  if (g->n == g->cap) {
    int cap = g->cap ? 2 * g->cap : 8;
    Node *a = realloc(g->nodes, (size_t)cap * sizeof *a);
    if (!a) return -1;
    g->nodes = a;
    g->cap = cap;
  }
  Node x = {NULL, malloc(sizeof value), NULL, 0, calloc((size_t)g->max_block, sizeof(float)), 0, 3, NULL, sizeof value, NULL, 0, NULL, CONTROL, 0, NULL, 0, 0, 0};
  if (!x.state || !x.out) {
    free(x.state);
    free(x.out);
    return -1;
  }
  memcpy(x.state, &value, sizeof value);
  g->nodes[g->n] = x;
  return g->n++;
}

/* a node of kind, without a kernel or state */
static int plain(funk_graph *g, int kind, int bus) {
  if (g->n == g->cap) {
    int cap = g->cap ? 2 * g->cap : 8;
    Node *a = realloc(g->nodes, (size_t)cap * sizeof *a);
    if (!a) return -1;
    g->nodes = a;
    g->cap = cap;
  }
  Node x = {NULL, NULL, NULL, 0, calloc((size_t)g->max_block, sizeof(float)), 0, mix(4 + (uint64_t)kind, (uint64_t)bus), NULL, 0, NULL, 0, NULL, kind, bus, NULL, 0, 0, 0};
  if (!x.out) return -1;
  g->nodes[g->n] = x;
  g->nbus += kind == BUS;
  return g->n++;
}

int funk_graph_bus(funk_graph *g, int orbit, int channel) {
  if (orbit < 0 || orbit >= FUNK_ORBITS || channel < 0 || channel >= FUNK_CHANNELS) return -1;
  return plain(g, BUS, orbit * FUNK_CHANNELS + channel);
}

int funk_graph_in(funk_graph *g, int channel) {
  if (channel < 0 || channel >= 2 * FUNK_CHANNELS) return -1; /* 2 and 3: an orbit effect's effect bus */
  return plain(g, IN, channel);
}

int funk_graph_output(funk_graph *g, int node) {
  if (node < 0 || node >= g->n) return -1;
  int *a = realloc(g->outs, (size_t)(g->nouts + 1) * sizeof *a);
  if (!a) return -1;
  g->outs = a;
  a[g->nouts] = node;
  return g->nouts++;
}

void funk_graph_free(funk_graph *g) {
  if (!g) return;
  for (int i = 0; i < g->n; i++) {
    free(g->nodes[i].init);
    free(g->nodes[i].state);
    free(g->nodes[i].in);
    free(g->nodes[i].out);
    free(g->nodes[i].mem);
    free(g->nodes[i].ix);
    funk_table_release(g->nodes[i].table);
  }
  free(g->nodes);
  free(g->names);
  free(g->outs);
  free(g->silence);
  free(g);
}

funk_player *funk_player_new(void) {
  funk_player *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  if (!(p->ev = funk_events_new())) {
    free(p);
    return NULL;
  }
  atomic_init(&p->current, NULL);
  atomic_init(&p->epoch, 0);
  atomic_init(&p->frames, 0);
  atomic_init(&p->head, 0);
  atomic_init(&p->tail, 0);
  return p;
}

void funk_player_reclaim(funk_player *p) {
  uint64_t now = atomic_load(&p->epoch);
  funk_events_reclaim(p->ev, now);
  for (funk_graph **q = &p->retired; *q;) {
    funk_graph *g = *q;
    if (now > g->retired + 1) {
      *q = g->next;
      funk_graph_free(g);
    } else {
      q = &g->next;
    }
  }
}

void funk_player_play(funk_player *p, funk_graph *g) {
  if (g) {
    name_shapes(g);
    for (int i = 0; i < g->n; i++) /* after names, so initial states compare without their pointers */
      if (g->nodes[i].mem) {
        memcpy(g->nodes[i].state, &g->nodes[i].mem, sizeof(void *));
        memcpy((char *)g->nodes[i].state + sizeof(void *), &g->nodes[i].frames, sizeof(int64_t));
      } else if (g->nodes[i].table) {
        funk_table_state(&g->nodes[i]);
      }
  }
  if (g) g->cx.rgen = funk_events_rgen(p->ev);
  funk_graph *old = atomic_exchange(&p->current, g);
  if (old) {
    old->retired = atomic_load(&p->epoch); /* read after the exchange */
    old->next = p->retired;
    p->retired = old;
  }
  funk_player_reclaim(p);
}

bool funk_player_send(funk_player *p, int64_t frame, const char *name, float value) {
  uint64_t head = atomic_load_explicit(&p->head, memory_order_relaxed);
  if (head - atomic_load_explicit(&p->tail, memory_order_acquire) == EVENTS) return false;
  p->events[head & (EVENTS - 1)] = (Event){frame, funk_hash(name), value};
  atomic_store_explicit(&p->head, head + 1, memory_order_release);
  return true;
}

bool funk_player_send_audio(funk_player *p, int64_t frame, const char *name, float value) {
  if (p->ahead - p->atail == EVENTS) return false;
  p->audio[p->ahead++ & (EVENTS - 1)] = (Event){frame, funk_hash(name), value};
  return true;
}

bool funk_player_has(funk_player *p, const char *name) {
  const funk_graph *g = atomic_load(&p->current);
  if (!g) return false;
  uint64_t key = funk_hash(name);
  int at = lower(g, key);
  return at < g->nnames && g->names[at].key == key && g->nodes[g->names[at].node].kind == CONTROL;
}

int64_t funk_player_frames(funk_player *p) { return atomic_load_explicit(&p->frames, memory_order_acquire); }

/* g's controls for the m frames from start: each holds its value until
   an event, which takes effect from its frame. Events after start + m wait.
   The three queues, the host's, the audio thread's and the event layer's,
   merge by frame: an event sets its control to the end of the block, so a
   later frame applied first would be overwritten. */
static void controls(funk_player *p, funk_graph *g, int64_t start, int m) {
  if (g)
    for (int i = 0; i < g->n; i++) {
      Node *x = &g->nodes[i];
      if (x->kind != CONTROL) continue;
      float v = *(float *)x->state;
      for (int j = 0; j < m; j++) x->out[j] = v;
    }
  uint64_t tail = atomic_load_explicit(&p->tail, memory_order_relaxed), head = atomic_load_explicit(&p->head, memory_order_acquire);
  for (;;) {
    const Event *h = tail != head ? &p->events[tail & (EVENTS - 1)] : NULL;
    const Event *a = p->atail != p->ahead ? &p->audio[p->atail & (EVENTS - 1)] : NULL;
    const funk_control_event *v = funk_events_control_peek(p->ev);
    Event ve;
    if (v) ve = (Event){v->frame, v->key, v->value};
    const Event *e = h && (!a || h->frame <= a->frame) ? h : a;
    if (v && (!e || ve.frame < e->frame)) e = &ve;
    if (!e || e->frame >= start + m) break;
    if (e == &ve) funk_events_control_pop(p->ev);
    else if (e == h) tail++;
    else p->atail++;
    if (!g) continue;
    int at = lower(g, e->key);
    if (at == g->nnames || g->names[at].key != e->key) continue;
    Node *x = &g->nodes[g->names[at].node];
    if (x->kind != CONTROL) continue;
    *(float *)x->state = e->value;
    for (int j = e->frame > start ? (int)(e->frame - start) : 0; j < m; j++) x->out[j] = e->value;
  }
  atomic_store_explicit(&p->tail, tail, memory_order_release);
}

void funk_player_render_channels(funk_player *p, float *const *out, int channels, int n) {
  funk_graph *g = atomic_load(&p->current);
  if (g != p->last) {
    if (g && p->last) carry(g, p->last);
    p->last = g;
  }
  int64_t start = atomic_load_explicit(&p->frames, memory_order_relaxed);
  for (int done = 0; done < n;) {
    int m = g ? (n - done < g->max_block ? n - done : g->max_block) : n - done;
    if (m > MAX_CHUNK) m = MAX_CHUNK;
    funk_events_render_to(p->ev, start + done + m);
    controls(p, g, start + done, m);
    if (!g || !g->n) {
      for (int c = 0; c < channels; c++) memset(out[c] + done, 0, (size_t)m * sizeof **out);
    } else {
      for (int i = 0; i < g->n; i++) {
        Node *x = &g->nodes[i];
        if (x->kind == BUS) funk_events_read(p->ev, x->bus / FUNK_CHANNELS, x->bus % FUNK_CHANNELS, start + done, m, x->out, 0);
      }
      for (int i = 0; i < g->n; i++) {
        Node *x = &g->nodes[i];
        if (x->k) x->k(x->state, x->in, x->out, m, &g->cx);
      }
      for (int c = 0; c < channels; c++)
        memcpy(out[c] + done, g->nodes[g->nouts ? g->outs[c % g->nouts] : g->n - 1].out, (size_t)m * sizeof **out);
    }
    if (!g || !g->nbus) /* no graph reads the orbits: they play as they are */
      for (int c = 0; c < channels; c++)
        for (int o = 0; o < FUNK_ORBITS; o++) funk_events_read(p->ev, o, c % FUNK_CHANNELS, start + done, m, out[c] + done, 1);
    done += m;
  }
  atomic_store_explicit(&p->frames, start + n, memory_order_release);
  atomic_fetch_add(&p->epoch, 1); /* after the last use of g */
}

void funk_player_render(funk_player *p, float *out, int n) { funk_player_render_channels(p, &out, 1, n); }

void funk_player_free(funk_player *p) {
  if (!p) return;
  funk_events_free(p->ev);
  funk_graph_free(atomic_load(&p->current));
  while (p->retired) {
    funk_graph *g = p->retired;
    p->retired = g->next;
    funk_graph_free(g);
  }
  free(p);
}
