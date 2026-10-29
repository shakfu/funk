/* graph.c -- see graph.h. The audio thread loads the current graph at each
   block's start and advances the epoch at its end. A graph replaced when
   the epoch was e may still be in the block running then, which ends by
   e + 1, and the next block copies named states from it, which ends by
   e + 2. So it is freed once the epoch exceeds e + 1. */
#include "graph.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  funk_kernel k;
  void *state;
  float **in; /* the inputs' buffers */
  int nin;
  float *out;
  uint64_t key; /* the name's hash, or 0 */
  void *init;   /* a named node's initial state */
  size_t size;
} Node;

typedef struct {
  uint64_t key;
  int node;
} Name;

struct funk_graph {
  Node *nodes;
  int n, cap, max_block;
  funk_context cx;
  float *silence;
  Name *names; /* sorted by key */
  int nnames;
  funk_graph *next; /* in the player's retired list */
  uint64_t retired; /* the epoch when it was replaced */
};

enum { EVENTS = 4096 }; /* a power of two */

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
};

funk_graph *funk_graph_new(int max_block, double rate) {
  if (max_block < 1 || !(rate > 0)) return NULL;
  funk_graph *g = calloc(1, sizeof *g);
  if (!g) return NULL;
  g->max_block = max_block;
  g->cx.rate = rate;
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
  Node x = {k, NULL, NULL, nin, NULL, 0, NULL, size};
  x.state = size ? calloc(1, size) : NULL;
  x.in = calloc((size_t)(nin ? nin : 1), sizeof *x.in);
  x.out = calloc((size_t)g->max_block, sizeof(float));
  if ((size && !x.state) || !x.in || !x.out) {
    free(x.state);
    free(x.in);
    free(x.out);
    return -1;
  }
  if (state && size) memcpy(x.state, state, size);
  for (int i = 0; i < nin; i++) x.in[i] = inputs[i] < 0 ? g->silence : g->nodes[inputs[i]].out;
  g->nodes[g->n] = x;
  return g->n++;
}

/* FNV-1a; 0 is kept for no name */
static uint64_t hash(const char *s) {
  uint64_t h = 14695981039346656037u;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211u;
  return h ? h : 1;
}

/* the first name whose key is at least key */
static int lower(const funk_graph *g, uint64_t key) {
  int lo = 0, hi = g->nnames;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (g->names[mid].key < key) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

int funk_graph_name(funk_graph *g, int node, const char *name) {
  if (node < 0 || node >= g->n || g->nodes[node].key || !name) return -1;
  Node *x = &g->nodes[node];
  uint64_t key = hash(name);
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
  return 0;
}

/* The audio thread, at g's first block: the states of old's nodes with a
   name, kernel and initial state that g's have. */
static void carry(funk_graph *g, const funk_graph *old) {
  for (int i = 0; i < g->nnames; i++) {
    int at = lower(old, g->names[i].key);
    if (at == old->nnames || old->names[at].key != g->names[i].key) continue;
    Node *x = &g->nodes[g->names[i].node];
    const Node *y = &old->nodes[old->names[at].node];
    if (x->k == y->k && x->size == y->size && (!x->size || !memcmp(x->init, y->init, x->size))) memcpy(x->state, y->state, x->size);
  }
}

int funk_graph_control(funk_graph *g, float value) {
  if (g->n == g->cap) {
    int cap = g->cap ? 2 * g->cap : 8;
    Node *a = realloc(g->nodes, (size_t)cap * sizeof *a);
    if (!a) return -1;
    g->nodes = a;
    g->cap = cap;
  }
  Node x = {NULL, malloc(sizeof value), NULL, 0, calloc((size_t)g->max_block, sizeof(float)), 0, NULL, sizeof value};
  if (!x.state || !x.out) {
    free(x.state);
    free(x.out);
    return -1;
  }
  memcpy(x.state, &value, sizeof value);
  g->nodes[g->n] = x;
  return g->n++;
}

void funk_graph_free(funk_graph *g) {
  if (!g) return;
  for (int i = 0; i < g->n; i++) {
    free(g->nodes[i].init);
    free(g->nodes[i].state);
    free(g->nodes[i].in);
    free(g->nodes[i].out);
  }
  free(g->nodes);
  free(g->names);
  free(g->silence);
  free(g);
}

funk_player *funk_player_new(void) {
  funk_player *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  atomic_init(&p->current, NULL);
  atomic_init(&p->epoch, 0);
  atomic_init(&p->frames, 0);
  atomic_init(&p->head, 0);
  atomic_init(&p->tail, 0);
  return p;
}

void funk_player_reclaim(funk_player *p) {
  uint64_t now = atomic_load(&p->epoch);
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
  p->events[head & (EVENTS - 1)] = (Event){frame, hash(name), value};
  atomic_store_explicit(&p->head, head + 1, memory_order_release);
  return true;
}

int64_t funk_player_frames(funk_player *p) { return atomic_load_explicit(&p->frames, memory_order_acquire); }

/* g's controls for the m frames from start: each holds its value until
   an event, which takes effect from its frame. Events after start + m wait. */
static void controls(funk_player *p, funk_graph *g, int64_t start, int m) {
  if (g)
    for (int i = 0; i < g->n; i++) {
      Node *x = &g->nodes[i];
      if (x->k) continue;
      float v = *(float *)x->state;
      for (int j = 0; j < m; j++) x->out[j] = v;
    }
  uint64_t tail = atomic_load_explicit(&p->tail, memory_order_relaxed), head = atomic_load_explicit(&p->head, memory_order_acquire);
  for (; tail != head; tail++) {
    const Event *e = &p->events[tail & (EVENTS - 1)];
    if (e->frame >= start + m) break;
    if (!g) continue;
    int at = lower(g, e->key);
    if (at == g->nnames || g->names[at].key != e->key) continue;
    Node *x = &g->nodes[g->names[at].node];
    if (x->k) continue;
    *(float *)x->state = e->value;
    for (int j = e->frame > start ? (int)(e->frame - start) : 0; j < m; j++) x->out[j] = e->value;
  }
  atomic_store_explicit(&p->tail, tail, memory_order_release);
}

void funk_player_render(funk_player *p, float *out, int n) {
  funk_graph *g = atomic_load(&p->current);
  if (g != p->last) {
    if (g && p->last) carry(g, p->last);
    p->last = g;
  }
  int64_t start = atomic_load_explicit(&p->frames, memory_order_relaxed);
  for (int done = 0; done < n;) {
    int m = g ? (n - done < g->max_block ? n - done : g->max_block) : n - done;
    controls(p, g, start + done, m);
    if (!g || !g->n) {
      memset(out + done, 0, (size_t)m * sizeof *out);
    } else {
      for (int i = 0; i < g->n; i++) {
        Node *x = &g->nodes[i];
        if (x->k) x->k(x->state, x->in, x->out, m, &g->cx);
      }
      memcpy(out + done, g->nodes[g->n - 1].out, (size_t)m * sizeof *out);
    }
    done += m;
  }
  atomic_store_explicit(&p->frames, start + n, memory_order_release);
  atomic_fetch_add(&p->epoch, 1); /* after the last use of g */
}

void funk_player_free(funk_player *p) {
  if (!p) return;
  funk_graph_free(atomic_load(&p->current));
  while (p->retired) {
    funk_graph *g = p->retired;
    p->retired = g->next;
    funk_graph_free(g);
  }
  free(p);
}
