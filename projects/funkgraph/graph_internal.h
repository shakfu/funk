/* graph_internal.h -- what graph.c and events.c share; not for hosts. */
#ifndef FUNK_GRAPH_INTERNAL_H
#define FUNK_GRAPH_INTERNAL_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "graph.h"
#include "table.h"

/* A node's kind. A control's output is a value events change; a bus reads
   an orbit's channel of the event layer; an input reads a template's chain. */
enum { KERNEL, CONTROL, BUS, IN };

typedef struct {
  funk_kernel k;
  void *state;
  float **in; /* the inputs' buffers */
  int nin;
  float *out;
  uint64_t key;   /* the name's hash, or 0 */
  uint64_t shape; /* its kernel's and inputs', without states */
  void *init;     /* a named node's initial state */
  size_t size;
  float *mem;     /* its delay memory, or NULL */
  int64_t frames;
  funk_table *table; /* the data it reads, or NULL */
  int kind;
  int bus;  /* a bus's orbit * FUNK_CHANNELS + channel; an input's channel */
  int *ix;  /* the inputs' indices, -1 for silence */
  uint64_t exp; /* named ">NAME": NAME's hash, which a template's later stages read */
  int ugen; /* funk_graph_ugen */
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
  int *outs; /* the channels' nodes */
  int nouts;
  int nbus;  /* its bus nodes */
  funk_graph *next; /* in the player's retired list */
  uint64_t retired; /* the epoch when it was replaced */
};

/* FNV-1a; 0 is kept for no name */
uint64_t funk_hash(const char *s);

/* The first 24 bytes of x's state: the data, its frames and its rate. */
void funk_table_state(Node *x);

/* The event layer (events.c), owned by a player. */
typedef struct funk_events funk_events;

funk_events *funk_player_events_of(funk_player *p);
uint64_t funk_player_epoch(funk_player *p);

funk_events *funk_events_new(void);

/* the event layer's random generator, which a played graph's kernels share */
double *funk_events_rgen(funk_events *ev);
void funk_events_free(funk_events *ev);

/* Host's thread: frees replaced template sets whose events have ended,
   once the epoch has passed the block they were replaced in. */
void funk_events_reclaim(funk_events *ev, uint64_t epoch);

/* The audio thread: renders the event layer's 64-frame blocks up to frame
   end, starting the events queued before each. */
void funk_events_render_to(funk_events *ev, int64_t end);

/* The audio thread: frames [start, start + m) of orbit's channel, written
   to out, or if add, added where an event played, so out keeps its bits. */
void funk_events_read(const funk_events *ev, int orbit, int channel, int64_t start, int m, float *out, int add);

/* The audio thread: the next write to a static graph's control that an
   event made (a global effect's parameter), if its frame is before end. */
typedef struct {
  int64_t frame;
  uint64_t key;
  float value;
} funk_control_event;
const funk_control_event *funk_events_control_peek(funk_events *ev);
void funk_events_control_pop(funk_events *ev);

#endif
