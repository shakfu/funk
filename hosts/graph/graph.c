/* graph.c -- see graph.h. The audio thread loads the current graph at each
   block's start and advances the epoch at its end. A graph replaced when
   the epoch was e may still be in the block running then, which ends by
   e + 1; no later block loads it. So it is freed once the epoch exceeds e. */
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
} Node;

struct funk_graph {
  Node *nodes;
  int n, cap, max_block;
  float *silence;
  funk_graph *next; /* in the player's retired list */
  uint64_t retired; /* the epoch when it was replaced */
};

struct funk_player {
  _Atomic(funk_graph *) current;
  _Atomic uint64_t epoch;
  funk_graph *retired;
};

funk_graph *funk_graph_new(int max_block) {
  if (max_block < 1) return NULL;
  funk_graph *g = calloc(1, sizeof *g);
  if (!g) return NULL;
  g->max_block = max_block;
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
  Node x = {k, NULL, NULL, nin, NULL};
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

void funk_graph_free(funk_graph *g) {
  if (!g) return;
  for (int i = 0; i < g->n; i++) {
    free(g->nodes[i].state);
    free(g->nodes[i].in);
    free(g->nodes[i].out);
  }
  free(g->nodes);
  free(g->silence);
  free(g);
}

funk_player *funk_player_new(void) {
  funk_player *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  atomic_init(&p->current, NULL);
  atomic_init(&p->epoch, 0);
  return p;
}

void funk_player_reclaim(funk_player *p) {
  uint64_t now = atomic_load(&p->epoch);
  for (funk_graph **q = &p->retired; *q;) {
    funk_graph *g = *q;
    if (now > g->retired) {
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

void funk_player_render(funk_player *p, float *out, int n) {
  funk_graph *g = atomic_load(&p->current);
  if (!g || !g->n) {
    memset(out, 0, (size_t)n * sizeof *out);
  } else {
    for (int done = 0; done < n;) {
      int m = n - done < g->max_block ? n - done : g->max_block;
      for (int i = 0; i < g->n; i++) {
        Node *x = &g->nodes[i];
        x->k(x->state, x->in, x->out, m);
      }
      memcpy(out + done, g->nodes[g->n - 1].out, (size_t)m * sizeof *out);
      done += m;
    }
  }
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
