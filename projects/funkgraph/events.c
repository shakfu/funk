/* events.c -- a player's event layer: sets of templates, each with a pool
   of slots, and events made from them on the audio thread; see graph.h
   and docs/dev/dirt-sound.md. Everything the audio thread touches is
   allocated on the host's thread: a stage is laid out in a free slot, so
   making one copies and points, and never calls malloc.

   Time: the layer renders blocks of B frames on a grid of the player's
   frames. An event starts in the block that holds its frame, and its
   output is delayed to that frame, the delay's last frames saved for the
   next block, as scsynth's OffsetOut does (IOUGens.cpp). An event that
   ends plays its saved frames in the block after, as OffsetOut_Dtor. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "graph_internal.h"

enum {
  B = FUNK_EVENT_BLOCK,
  RING = 8192,  /* frames of each orbit's bus, a power of two: twice graph.c's MAX_CHUNK */
  QUEUE = 256,  /* events, a power of two */
  PARAMS = 48,  /* an event's parameters */
  LIVE = 256,   /* events playing at once */
  STAGES = 16,  /* an event's source and modules */
  CONTROLQ = 4096, /* writes to the played graph's controls, a power of two */
  CLASSES = 48,   /* size classes: a template's bytes round up to a power of two */
  FXPARAMS = 16   /* an orbit effect's parameters */
};

/* A size class's slots, at most this many bytes, unless that is under
   one: scsynth's default real-time memory (ServerOptions.memSize) */
#define CLASS_BYTES ((size_t)8192 * 1024)

typedef struct {
  uint64_t key, okey; /* "NAME" and "oORBIT.NAME" */
  float value;
} Param;

typedef struct {
  int64_t frame;
  uint64_t source;
  int orbit, n;
  Param p[PARAMS];
} Rec;

/* a template's node, laid out in a slot */
typedef struct {
  funk_kernel k;
  void *state;
  float **in;
  float *out;
  uint64_t key, exp;
  int kind, ugen;
} INode;

typedef struct {
  INode *nodes;
  int n;
  float *outs[FUNK_CHANNELS];
  int begun, freed; /* its UGens constructed; freed by doneAction 2 */
} Stage;

typedef struct {
  uint64_t key, flag; /* the name's hash; a module's "~NAME" */
  int module;
  funk_graph *g;
  size_t bytes; /* in a slot */
  int cls; /* its size class */
  /* an orbit's effect: 1 resumed by its parameters, 2 always */
  int fx, nouts, np, from;
  uint64_t pkeys[FXPARAMS], sig;
  struct Fx *inst; /* one per orbit */
} Template;

/* an orbit's instance of an effect, as a GlobalDirtEffect's synth */
typedef struct Fx {
  unsigned char *mem;
  Stage st;
  float *outs[2 * FUNK_CHANNELS];
  int run;
  int have[FXPARAMS]; /* the last parameters an event set, as GlobalDirtEffect's state */
  float val[FXPARAMS];
  funk_context cx;
} Fx;

/* the templates whose bytes round up to one power of two share a pool, of
   slots as large as the largest of them */
typedef struct {
  size_t slot; /* bytes per slot */
  int slots;
  unsigned char *pool;
  int *free, nfree; /* the audio thread's */
} Class;

struct funk_templates {
  Template *t;
  int n, cap;
  int slots; /* a class's most */
  Class c[CLASSES];
  funk_templates *parent; /* the set it replaced, whose effects it may keep */
  int adopted;            /* the audio thread's: its effects took the parent's */
  _Atomic int live; /* stages of this set still playing */
  uint64_t retired; /* the epoch when it was replaced */
  funk_templates *next;
};

typedef struct {
  funk_templates *set;
  int nst;
  int slot[STAGES], cls[STAGES];
  Stage st[STAGES];
  funk_context cx;
  int offset; /* frames into its first block */
  int orbit;
  float cut; /* its cut group's size, or 0 */
  uint64_t source;
  int ending; /* done: its saved frames play in the next block */
  float saved[FUNK_CHANNELS][B];
} Ev;

struct funk_events {
  _Atomic(funk_templates *) current;
  funk_templates *retired; /* the host's */
  _Atomic uint64_t head, tail; /* the host's queue */
  Rec q[QUEUE];
  uint64_t ahead, atail; /* the audio thread's queue */
  Rec aq[QUEUE];
  int64_t end; /* frames rendered into the buses, a multiple of B */
  float bus[FUNK_ORBITS][FUNK_CHANNELS][RING];
  unsigned char used[FUNK_ORBITS][RING / B]; /* an event wrote the block: else it is silence */
  float chain[FUNK_CHANNELS][B], eff[FUNK_CHANNELS][B], tmp[2 * FUNK_CHANNELS][B], silence[B];
  funk_templates *fxset; /* the audio thread's: whose effects the orbits run */
  double rgen[3];        /* scsynth's world RGen, each a uint32 */
  _Atomic uint64_t seed; /* a seed for the next block, with bit 32 set */
  uint64_t blocks;
  Ev ev[LIVE]; /* in the order they started */
  int nev;
  funk_control_event cq[CONTROLQ];
  uint64_t chead, ctail;
  uint64_t k_orbit, k_cut, k_stop, k_resumed;
  struct { /* the host's event in parts */
    int64_t frame;
    char source[128], names[PARAMS][64];
    const char *np[PARAMS];
    float values[PARAMS];
    int n;
    bool ok;
  } part;
  _Atomic int live;
  _Atomic uint64_t dropped, late;
};

static size_t al(size_t n) { return (n + 15) & ~(size_t)15; }

/* RGen::init (SC_RGen.h), with Hash's Thomas Wang hash (Hash.h) */
static void rgen_init(double *r, uint32_t seed) {
  uint32_t h = seed;
  h += ~(h << 15);
  h ^= h >> 10;
  h += h << 3;
  h ^= h >> 6;
  h += ~(h << 11);
  h ^= h >> 16;
  uint32_t s1 = 1243598713U ^ h, s2 = 3093459404U ^ h, s3 = 1821928721U ^ h;
  r[0] = s1 < 2 ? 1243598713U : s1;
  r[1] = s2 < 8 ? 3093459404U : s2;
  r[2] = s3 < 16 ? 1821928721U : s3;
}

double *funk_events_rgen(funk_events *e) { return e->rgen; }

void funk_player_seed(funk_player *p, uint32_t seed) {
  atomic_store(&funk_player_events_of(p)->seed, (uint64_t)seed | (1ULL << 32));
}

funk_events *funk_events_new(void) {
  funk_events *e = malloc(sizeof *e);
  if (!e) return NULL;
  memset(e, 0, sizeof *e); /* over calloc: the audio thread must not fault its pages in */
  atomic_init(&e->current, NULL);
  atomic_init(&e->head, 0);
  atomic_init(&e->tail, 0);
  atomic_init(&e->live, 0);
  atomic_init(&e->dropped, 0);
  atomic_init(&e->seed, 0);
  rgen_init(e->rgen, (uint32_t)time(NULL) ^ (uint32_t)clock());
  atomic_init(&e->late, 0);
  e->k_orbit = funk_hash("orbit");
  e->k_cut = funk_hash("cut");
  e->k_stop = funk_hash("stop");
  e->k_resumed = funk_hash("resumed");
  return e;
}

static void set_free(funk_templates *t) {
  for (int i = 0; i < t->n; i++) {
    if (t->t[i].inst)
      for (int o = 0; o < FUNK_ORBITS; o++) free(t->t[i].inst[o].mem);
    free(t->t[i].inst);
    funk_graph_free(t->t[i].g);
  }
  free(t->t);
  for (int i = 0; i < CLASSES; i++) {
    free(t->c[i].pool);
    free(t->c[i].free);
  }
  free(t);
}

void funk_events_free(funk_events *e) {
  if (!e) return;
  funk_templates *t = atomic_load(&e->current);
  if (t) set_free(t);
  while (e->retired) {
    t = e->retired;
    e->retired = t->next;
    set_free(t);
  }
  free(e);
}

void funk_events_reclaim(funk_events *e, uint64_t epoch) {
  for (funk_templates **q = &e->retired; *q;) {
    funk_templates *t = *q;
    if (epoch > t->retired + 1 && atomic_load_explicit(&t->live, memory_order_acquire) == 0) {
      *q = t->next;
      set_free(t);
    } else {
      q = &t->next;
    }
  }
}

funk_templates *funk_templates_new(int slots) {
  if (slots < 1) return NULL;
  funk_templates *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->slots = slots;
  atomic_init(&t->live, 0);
  return t;
}

void funk_templates_free(funk_templates *t) {
  if (t) set_free(t);
}

/* bytes of g's nodes laid out in a slot */
static size_t layout(const funk_graph *g) {
  size_t b = al((size_t)g->n * sizeof(INode));
  for (int i = 0; i < g->n; i++) {
    const Node *x = &g->nodes[i];
    b += al(x->size) + al((size_t)x->nin * sizeof(float *)) + al((size_t)x->frames * sizeof(float));
    if (x->kind != IN) b += al(B * sizeof(float));
  }
  return b;
}

int funk_templates_add(funk_templates *t, const char *name, funk_graph *g) {
  uint64_t key = funk_hash(name);
  bool ok = g && g->max_block >= B && !g->nbus && g->n > 0;
  for (int i = 0; ok && i < t->n; i++) ok = t->t[i].key != key;
  if (ok && t->n == t->cap) {
    int cap = t->cap ? 2 * t->cap : 8;
    Template *a = realloc(t->t, (size_t)cap * sizeof *a);
    if (a) t->t = a, t->cap = cap;
    else ok = false;
  }
  if (!ok) {
    funk_graph_free(g);
    return -1;
  }
  Template x = {.key = key, .module = !strncmp(name, "m.", 2), .g = g, .bytes = layout(g)};
  while (x.cls < CLASSES - 1 && ((size_t)1 << x.cls) < x.bytes) x.cls++;
  x.fx = !strncmp(name, "o.", 2) ? 1 : !strncmp(name, "a.", 2) ? 2 : 0;
  x.from = -1;
  if (x.fx) {
    /* its parameters: its controls, but "resumed"; its signature: the whole graph */
    uint64_t resumed = funk_hash("resumed"), h = 1469598103934665603ULL;
    x.nouts = g->nouts;
    ok = x.nouts == 2 || x.nouts == 4;
    for (int i = 0; ok && i < g->n; i++) {
      const Node *y = &g->nodes[i];
      if (y->kind == CONTROL && y->key != resumed) {
        ok = x.np < FXPARAMS;
        if (ok) x.pkeys[x.np++] = y->key;
      }
      uint64_t w[7] = {(uint64_t)(uintptr_t)y->k, (uint64_t)y->kind, y->key, (uint64_t)y->size, (uint64_t)y->frames, (uint64_t)y->bus, (uint64_t)y->nin};
      for (int k = 0; k < 7; k++) h = (h ^ w[k]) * 1099511628211ULL;
      for (int k = 0; k < y->nin; k++) h = (h ^ (uint64_t)(int64_t)y->ix[k]) * 1099511628211ULL;
      for (size_t k = 0; k < y->size; k++) h = (h ^ ((const unsigned char *)y->state)[k]) * 1099511628211ULL;
    }
    for (int i = 0; i < g->nouts; i++) h = (h ^ (uint64_t)g->outs[i]) * 1099511628211ULL;
    x.sig = h ^ key;
    if (!ok) {
      funk_graph_free(g);
      return -1;
    }
  }
  if (x.module) {
    char flag[128];
    snprintf(flag, sizeof flag, "~%s", name + 2);
    x.flag = funk_hash(flag);
  }
  for (int i = 0; i < g->n; i++)
    if (g->nodes[i].table) funk_table_state(&g->nodes[i]);
  t->t[t->n++] = x;
  return 0;
}

static void make(funk_events *e, const Template *t, unsigned char *m, Stage *s);

int funk_player_templates(funk_player *p, funk_templates *t) {
  funk_events *e = funk_player_events_of(p);
  if (t) {
    for (int i = 0; i < t->n; i++) {
      Class *c = &t->c[t->t[i].cls];
      if (!t->t[i].fx && t->t[i].bytes > c->slot) c->slot = t->t[i].bytes;
    }
    /* each orbit's effects: kept from the set this replaces if unchanged, else made */
    funk_templates *old = atomic_load_explicit(&e->current, memory_order_relaxed);
    t->parent = old;
    for (int i = 0; i < t->n; i++) {
      Template *x = &t->t[i];
      if (!x->fx) continue;
      for (int j = 0; old && j < old->n; j++)
        if (old->t[j].fx == x->fx && old->t[j].sig == x->sig) x->from = j;
      if (x->from >= 0) continue;
      x->inst = calloc(FUNK_ORBITS, sizeof *x->inst);
      if (!x->inst) {
        set_free(t);
        return -1;
      }
      for (int o = 0; o < FUNK_ORBITS; o++) {
        Fx *f = &x->inst[o];
        if (!(f->mem = malloc(x->bytes))) {
          set_free(t);
          return -1;
        }
        make(e, x, f->mem, &f->st);
        for (int c = 0; c < x->nouts; c++) f->outs[c] = f->st.nodes[x->g->outs[c]].out;
        f->run = 1; /* its first block, as a synth's first calc, before PauseSelf pauses it */
        f->cx = (funk_context){x->g->cx.rate, 0, 0, e->rgen, 0, 0};
      }
    }
    for (int k = 0; k < CLASSES; k++) {
      Class *c = &t->c[k];
      if (!c->slot) continue;
      size_t most = CLASS_BYTES / c->slot;
      c->slots = most < 1 ? 1 : most < (size_t)t->slots ? (int)most : t->slots;
      c->pool = malloc((size_t)c->slots * c->slot);
      c->free = malloc((size_t)c->slots * sizeof *c->free);
      if (!c->pool || !c->free) {
        set_free(t);
        return -1;
      }
      memset(c->pool, 0, (size_t)c->slots * c->slot);
      for (int i = 0; i < c->slots; i++) c->free[i] = c->slots - 1 - i;
      c->nfree = c->slots;
    }
  }
  funk_templates *old = atomic_exchange(&e->current, t);
  if (old) {
    old->retired = funk_player_epoch(p); /* read after the exchange */
    old->next = e->retired;
    e->retired = old;
  }
  funk_player_reclaim(p);
  return 0;
}

static int find(const funk_templates *t, uint64_t key) {
  for (int i = 0; i < t->n; i++)
    if (t->t[i].key == key) return i;
  return -1;
}

bool funk_player_has_template(funk_player *p, const char *name) {
  const funk_templates *t = atomic_load(&funk_player_events_of(p)->current);
  return t && find(t, funk_hash(name)) >= 0;
}

static bool fill(Rec *r, int64_t frame, const char *source, int n, const char *const *names, const float *values) {
  if (n < 0 || n > PARAMS) return false;
  int orbit = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(names[i], "orbit") && isfinite(values[i])) orbit = (int)values[i];
  orbit %= FUNK_ORBITS; /* SuperDirt's orbits.wrapAt */
  if (orbit < 0) orbit += FUNK_ORBITS;
  r->frame = frame;
  r->source = funk_hash(source);
  r->orbit = orbit;
  r->n = n;
  for (int i = 0; i < n; i++) {
    char o[128];
    snprintf(o, sizeof o, "o%d.%s", orbit, names[i]);
    r->p[i] = (Param){funk_hash(names[i]), funk_hash(o), values[i]};
  }
  return true;
}

bool funk_player_event(funk_player *p, int64_t frame, const char *source, int n, const char *const *names, const float *values) {
  funk_events *e = funk_player_events_of(p);
  uint64_t head = atomic_load_explicit(&e->head, memory_order_relaxed);
  if (head - atomic_load_explicit(&e->tail, memory_order_acquire) == QUEUE) return false;
  if (!fill(&e->q[head & (QUEUE - 1)], frame, source, n, names, values)) return false;
  atomic_store_explicit(&e->head, head + 1, memory_order_release);
  return true;
}

bool funk_player_event_audio(funk_player *p, int64_t frame, const char *source, int n, const char *const *names,
                             const float *values) {
  funk_events *e = funk_player_events_of(p);
  if (e->ahead - e->atail == QUEUE || !fill(&e->aq[e->ahead & (QUEUE - 1)], frame, source, n, names, values)) return false;
  e->ahead++;
  return true;
}

bool funk_player_event_begin(funk_player *p, int64_t frame, const char *source) {
  funk_events *e = funk_player_events_of(p);
  e->part.frame = frame;
  e->part.n = 0;
  e->part.ok = (size_t)snprintf(e->part.source, sizeof e->part.source, "%s", source) < sizeof e->part.source;
  return e->part.ok;
}

bool funk_player_event_param(funk_player *p, const char *name, float value) {
  funk_events *e = funk_player_events_of(p);
  int i = e->part.n;
  if (i == PARAMS || (size_t)snprintf(e->part.names[i], sizeof e->part.names[i], "%s", name) >= sizeof e->part.names[i]) return e->part.ok = false;
  e->part.np[i] = e->part.names[i];
  e->part.values[i] = value;
  e->part.n++;
  return true;
}

bool funk_player_event_end(funk_player *p) {
  funk_events *e = funk_player_events_of(p);
  return e->part.ok && funk_player_event(p, e->part.frame, e->part.source, e->part.n, e->part.np, e->part.values);
}

void funk_player_event_counts(funk_player *p, int *live, uint64_t *dropped, uint64_t *late) {
  funk_events *e = funk_player_events_of(p);
  if (live) *live = atomic_load(&e->live);
  if (dropped) *dropped = atomic_load(&e->dropped);
  if (late) *late = atomic_load(&e->late);
}

/* t's template laid out in slot memory m */
static void make(funk_events *e, const Template *t, unsigned char *m, Stage *s) {
  const funk_graph *g = t->g;
  INode *nd = (INode *)m;
  unsigned char *q = m + al((size_t)g->n * sizeof *nd);
  for (int i = 0; i < g->n; i++) {
    const Node *x = &g->nodes[i];
    INode *y = &nd[i];
    y->k = x->k;
    y->kind = x->kind;
    y->ugen = x->ugen;
    y->key = x->key;
    y->exp = x->exp;
    y->state = q;
    q += al(x->size);
    if (x->size) memcpy(y->state, x->state, x->size);
    y->in = (float **)q;
    q += al((size_t)x->nin * sizeof(float *));
    if (x->kind == IN) {
      y->out = x->bus < FUNK_CHANNELS ? e->chain[x->bus] : e->eff[x->bus - FUNK_CHANNELS];
    } else {
      y->out = (float *)q;
      q += al(B * sizeof(float));
    }
    for (int j = 0; j < x->nin; j++) y->in[j] = x->ix[j] < 0 ? e->silence : nd[x->ix[j]].out;
    if (x->mem) { /* as funk_player_play gives a graph's: the pointer and the frames */
      float *mem = (float *)q;
      q += al((size_t)x->frames * sizeof(float));
      memset(mem, 0, (size_t)x->frames * sizeof(float));
      memcpy(y->state, &mem, sizeof mem);
      memcpy((char *)y->state + sizeof mem, &x->frames, sizeof x->frames);
    }
  }
  for (int c = 0; c < FUNK_CHANNELS; c++) s->outs[c] = nd[g->nouts ? g->outs[c % g->nouts] : g->n - 1].out;
  s->nodes = nd;
  s->n = g->n;
  s->begun = s->freed = 0;
}

static void set_param(Ev *v, uint64_t key, float value) {
  for (int i = 0; i < v->nst; i++)
    for (int j = 0; j < v->st[i].n; j++) {
      INode *y = &v->st[i].nodes[j];
      if (y->kind == CONTROL && y->key == key) memcpy(y->state, &value, sizeof value);
    }
}

static void drop(funk_events *e) { atomic_fetch_add_explicit(&e->dropped, 1, memory_order_relaxed); }

static void set_control(Stage *s, uint64_t key, float value) {
  for (int j = 0; j < s->n; j++)
    if (s->nodes[j].kind == CONTROL && s->nodes[j].key == key) memcpy(s->nodes[j].state, &value, sizeof value);
}

/* r's orbit's effects, as GlobalDirtEffect.set: a parameter that differs
   from the last is set, to 0 if r lacks it, as sclang sends nil; one that
   r has, or an "a." effect, resumes the effect, with "resumed" set */
static void set_effects(funk_events *e, const Rec *r) {
  funk_templates *t = e->fxset;
  if (!t) return;
  for (int i = 0; i < t->n; i++) {
    Template *x = &t->t[i];
    if (!x->fx) continue;
    Fx *f = &x->inst[r->orbit];
    bool some = x->fx == 2;
    for (int k = 0; k < x->np; k++) {
      int have = 0;
      float v = 0;
      for (int j = 0; j < r->n; j++)
        if (r->p[j].key == x->pkeys[k]) {
          have = 1;
          v = r->p[j].value;
        }
      some |= have;
      if (have != f->have[k] || (have && v != f->val[k])) {
        set_control(&f->st, x->pkeys[k], v);
        f->have[k] = have;
        f->val[k] = v;
      }
    }
    if (some) {
      f->run = 1;
      set_control(&f->st, e->k_resumed, 1);
    }
  }
}

/* r's event, in the block from frame b */
static void start(funk_events *e, const Rec *r, int64_t b) {
  funk_templates *t = atomic_load_explicit(&e->current, memory_order_acquire);
  int src = t ? find(t, r->source) : -1;
  if (src < 0 || t->t[src].module || t->t[src].fx) {
    drop(e);
    return;
  }
  set_effects(e, r);
  int idx[STAGES], n = 0;
  idx[n++] = src;
  for (int i = 0; i < t->n && n < STAGES; i++) {
    if (!t->t[i].module) continue;
    for (int j = 0; j < r->n; j++)
      if (r->p[j].key == t->t[i].flag && r->p[j].value != 0) {
        idx[n++] = i;
        break;
      }
  }
  /* a free slot of its class for each stage */
  int need[CLASSES] = {0};
  bool room = e->nev < LIVE;
  for (int i = 0; i < n; i++) {
    int k = t->t[idx[i]].cls;
    room = room && ++need[k] <= t->c[k].nfree;
  }
  if (!room) {
    drop(e);
    return;
  }
  float cut = 0;
  for (int j = 0; j < r->n; j++)
    if (r->p[j].key == e->k_cut) cut = r->p[j].value;
  /* a cut group, as SuperDirt's: an event of the orbit with this cut's
     size stops, of any source for a positive cut, of this one for a negative */
  if (cut != 0)
    for (int i = 0; i < e->nev; i++) {
      Ev *w = &e->ev[i];
      if (!w->ending && w->orbit == r->orbit && w->cut == fabsf(cut) && (cut > 0 || w->source == r->source)) set_param(w, e->k_stop, 1);
    }
  if (r->frame < b) atomic_fetch_add_explicit(&e->late, 1, memory_order_relaxed);
  Ev *v = &e->ev[e->nev++];
  v->set = t;
  v->nst = n;
  for (int i = 0; i < n; i++) {
    Class *c = &t->c[t->t[idx[i]].cls];
    v->cls[i] = t->t[idx[i]].cls;
    v->slot[i] = c->free[--c->nfree];
    make(e, &t->t[idx[i]], c->pool + (size_t)v->slot[i] * c->slot, &v->st[i]);
  }
  for (int j = 0; j < r->n; j++) set_param(v, r->p[j].key, r->p[j].value);
  v->cx = (funk_context){t->t[src].g->cx.rate, 0, 0, e->rgen, 0, 0};
  v->offset = r->frame > b ? (int)(r->frame - b) : 0;
  v->orbit = r->orbit;
  v->cut = fabsf(cut);
  v->source = r->source;
  v->ending = 0;
  memset(v->saved, 0, sizeof v->saved);
  atomic_fetch_add_explicit(&t->live, n, memory_order_relaxed);
  atomic_fetch_add_explicit(&e->live, 1, memory_order_relaxed);
  /* the played graph's controls of the orbit, a global effect's, from its frame */
  for (int j = 0; j < r->n; j++) {
    if (e->chead - e->ctail == CONTROLQ) break;
    e->cq[e->chead++ & (CONTROLQ - 1)] = (funk_control_event){r->frame, r->p[j].okey, r->p[j].value};
  }
}

static void end(funk_events *e, Ev *v) {
  funk_templates *t = v->set;
  for (int i = 0; i < v->nst; i++) {
    Class *c = &t->c[v->cls[i]];
    c->free[c->nfree++] = v->slot[i];
  }
  atomic_fetch_sub_explicit(&t->live, v->nst, memory_order_release);
  atomic_fetch_sub_explicit(&e->live, 1, memory_order_relaxed);
}

/* stage k of v; a node named ">NAME" then sets parameter NAME of the stages
   after, as SuperDirt's modules and dirt_gate read the sustain DirtEvent computed */
/* s's UGens' constructors, in order, as scsynth's Graph_FirstCalc, once */
static void construct(Stage *s, funk_context *cx) {
  if (s->begun) return;
  s->begun = 1;
  cx->ctor = 1;
  for (int i = 0; i < s->n; i++) {
    INode *y = &s->nodes[i];
    if (y->kind == CONTROL) memcpy(y->out, y->state, sizeof(float));
    else if (y->kind == KERNEL && y->ugen) y->k(y->state, y->in, y->out, 1, cx);
  }
  cx->ctor = 0;
}

static void run(funk_events *e, Ev *v, int k) {
  Stage *s = &v->st[k];
  if (s->freed) return;
  construct(s, &v->cx);
  for (int i = 0; i < s->n; i++) {
    INode *y = &s->nodes[i];
    if (y->kind == CONTROL) {
      float x;
      memcpy(&x, y->state, sizeof x);
      for (int j = 0; j < B; j++) y->out[j] = x;
    } else if (y->kind == KERNEL) {
      y->k(y->state, y->in, y->out, B, &v->cx);
    }
  }
  for (int i = 0; i < s->n; i++) {
    const INode *y = &s->nodes[i];
    if (!y->exp) continue;
    for (int t = k + 1; t < v->nst; t++)
      for (int j = 0; j < v->st[t].n; j++) {
        INode *z = &v->st[t].nodes[j];
        if (z->kind == CONTROL && z->key == y->exp) memcpy(z->state, &y->out[0], sizeof(float));
      }
  }
  for (int c = 0; c < FUNK_CHANNELS; c++) memcpy(e->tmp[c], s->outs[c], sizeof e->tmp[c]); /* an output may be an input */
  memcpy(e->chain, e->tmp, sizeof e->chain);
  if (v->cx.freeself != 0) {
    s->freed = 1;
    v->cx.freeself = 0;
  }
}

/* the queued record with the earliest frame, from either queue */
static const Rec *next(funk_events *e, uint64_t tail, uint64_t head, bool *host) {
  const Rec *h = tail != head ? &e->q[tail & (QUEUE - 1)] : NULL;
  const Rec *a = e->atail != e->ahead ? &e->aq[e->atail & (QUEUE - 1)] : NULL;
  *host = h && (!a || h->frame <= a->frame);
  return *host ? h : a;
}

/* orbit o's effects on its dry bus, block at: each running one replaces
   the dry and effect buses, or as the monitor, gives the orbit's output.
   Without a monitor, the output is dry plus effect; with a paused one,
   silence. A paused effect does nothing, as a paused synth. */
static void effects(funk_events *e, int o, size_t at) {
  funk_templates *t = e->fxset;
  if (!t) return;
  bool monitor = false, ran = false, sounds = e->used[o][at / B];
  for (int c = 0; c < FUNK_CHANNELS; c++) {
    memcpy(e->chain[c], &e->bus[o][c][at], sizeof e->chain[c]);
    memset(e->eff[c], 0, sizeof e->eff[c]);
  }
  float mon[FUNK_CHANNELS][B];
  memset(mon, 0, sizeof mon);
  for (int i = 0; i < t->n; i++) {
    Template *x = &t->t[i];
    if (!x->fx) continue;
    monitor |= x->nouts == 2;
    Fx *f = &x->inst[o];
    if (!f->run) continue;
    ran = true;
    f->cx.block = (double)e->blocks;
    Stage *s = &f->st;
    construct(s, &f->cx);
    for (int j = 0; j < s->n; j++) {
      INode *y = &s->nodes[j];
      if (y->kind == CONTROL) {
        float v;
        memcpy(&v, y->state, sizeof v);
        for (int k = 0; k < B; k++) y->out[k] = v;
      } else if (y->kind == KERNEL) {
        y->k(y->state, y->in, y->out, B, &f->cx);
      }
    }
    set_control(s, e->k_resumed, 0); /* a TrigControl: one block */
    for (int c = 0; c < x->nouts; c++) memcpy(e->tmp[c], f->outs[c], sizeof e->tmp[c]); /* an output may be an input */
    if (x->nouts == 2) {
      memcpy(mon, e->tmp, sizeof mon);
    } else {
      memcpy(e->chain, e->tmp, sizeof e->chain);
      memcpy(e->eff, e->tmp[FUNK_CHANNELS], sizeof e->eff);
    }
    if (f->cx.done != 0) { /* doneAction 1: paused from the next block */
      f->run = 0;
      f->cx.done = 0;
    }
  }
  if (!ran && !sounds) return;
  for (int c = 0; c < FUNK_CHANNELS; c++)
    for (int j = 0; j < B; j++) e->bus[o][c][at + j] = monitor ? mon[c][j] : e->chain[c][j] + e->eff[c][j];
  e->used[o][at / B] = 1;
}

/* t's effects, taking its parent's where t kept them, its parent's first
   if the audio thread never ran them */
static void adopt(funk_events *e, funk_templates *t) {
  if (t && !t->adopted) {
    if (t->parent && !t->parent->adopted) adopt(e, t->parent);
    for (int i = 0; i < t->n; i++) {
      Template *x = &t->t[i];
      if (x->fx && x->from >= 0) {
        x->inst = t->parent->t[x->from].inst;
        t->parent->t[x->from].inst = NULL;
      }
    }
    t->adopted = 1;
  }
  e->fxset = t;
}

static void block(funk_events *e, int64_t b) {
  uint64_t tail = atomic_load_explicit(&e->tail, memory_order_relaxed), head = atomic_load_explicit(&e->head, memory_order_acquire);
  for (;;) {
    bool host;
    const Rec *r = next(e, tail, head, &host);
    if (!r || r->frame >= b + B) break;
    start(e, r, b);
    if (host) tail++;
    else e->atail++;
  }
  atomic_store_explicit(&e->tail, tail, memory_order_release);
  size_t at = (size_t)(b & (RING - 1));
  for (int o = 0; o < FUNK_ORBITS; o++) {
    if (e->used[o][at / B])
      for (int c = 0; c < FUNK_CHANNELS; c++) memset(&e->bus[o][c][at], 0, B * sizeof(float));
    e->used[o][at / B] = 0;
  }
  /* scsynth's node order: each orbit's group, the last orbit's first
     (DirtOrbit.initNodeTree adds each at the head), then its effects */
  for (int o = FUNK_ORBITS - 1; o >= 0; o--) {
    for (int i = 0; i < e->nev; i++) {
      Ev *v = &e->ev[i];
      if (v->orbit != o) continue;
      int off = v->offset;
      bool over = v->ending;
      e->used[o][at / B] = 1;
      if (!over) {
        memset(e->chain, 0, sizeof e->chain);
        v->cx.block = (double)e->blocks;
        for (int s = 0; s < v->nst; s++) run(e, v, s);
      }
      for (int c = 0; c < FUNK_CHANNELS; c++) {
        float *out = &e->bus[o][c][at];
        for (int j = 0; j < off; j++) out[j] += v->saved[c][j];
        if (over) continue;
        for (int j = 0; j < B - off; j++) out[off + j] += e->chain[c][j];
        memcpy(v->saved[c], e->chain[c] + B - off, (size_t)off * sizeof(float));
      }
      if (!over && v->cx.done != 0 && off > 0) v->ending = 1;
      else if (over || v->cx.done != 0) v->ending = 2; /* ends below */
    }
    effects(e, o, at);
  }
  int kept = 0;
  for (int i = 0; i < e->nev; i++) {
    Ev *v = &e->ev[i];
    if (v->ending == 2) {
      end(e, v);
      continue;
    }
    if (kept != i) e->ev[kept] = *v;
    kept++;
  }
  e->nev = kept;
  e->blocks++;
}

void funk_events_render_to(funk_events *e, int64_t to) {
  /* in every render, so a set is adopted before its parent can be reclaimed */
  funk_templates *t = atomic_load_explicit(&e->current, memory_order_acquire);
  if (t != e->fxset) adopt(e, t);
  uint64_t seed = atomic_exchange_explicit(&e->seed, 0, memory_order_relaxed);
  if (seed) rgen_init(e->rgen, (uint32_t)seed);
  while (e->end < to) {
    block(e, e->end);
    e->end += B;
  }
}

void funk_events_read(const funk_events *e, int orbit, int channel, int64_t start, int m, float *out, int add) {
  const float *bus = e->bus[orbit][channel];
  for (int j = 0; j < m; j++) {
    size_t at = (size_t)(start + j) & (RING - 1);
    if (add && !e->used[orbit][at / B]) continue; /* nothing played: out keeps its bits, -0 included */
    out[j] = add ? out[j] + bus[at] : bus[at];
  }
}

const funk_control_event *funk_events_control_peek(funk_events *e) {
  return e->chead != e->ctail ? &e->cq[e->ctail & (CONTROLQ - 1)] : NULL;
}

void funk_events_control_pop(funk_events *e) { e->ctail++; }
