/* graph.h -- runs a graph of kernels on an audio thread (docs/dev/audio.md, D).

   A kernel is a foreign export emitted by `funk emit-c --kernels`, with the
   signature funk_kernel. A graph is built on the host's thread: each node
   is a kernel, a copy of its initial state, and inputs from earlier nodes,
   so a graph has no cycles. The last node added is the output. Everything
   a node needs is allocated when it is added; rendering allocates nothing.

   A player renders one graph at a time on one audio thread. funk_player_play
   replaces it from the host's thread at the next block; the old graph is
   freed by a later funk_player_reclaim, once the audio thread has finished
   a block without it. */
#ifndef FUNK_GRAPH_H
#define FUNK_GRAPH_H

#include <stddef.h>

typedef void (*funk_kernel)(void *state, float **in, float *out, int n);

typedef struct funk_graph funk_graph;
typedef struct funk_player funk_player;

/* A graph rendering at most max_block frames per kernel call; NULL if
   memory runs out. */
funk_graph *funk_graph_new(int max_block);

/* Adds a node: kernel k, size bytes of state copied from state (or zeroed
   if state is NULL), and nin inputs, each an earlier node's index or -1 for
   silence. The node's index, or -1 if an input is not an earlier node or
   memory runs out. Not after the graph is played. */
int funk_graph_add(funk_graph *g, funk_kernel k, const void *state, size_t size, const int *inputs, int nin);

/* A graph that was never played. */
void funk_graph_free(funk_graph *g);

funk_player *funk_player_new(void);

/* Host's thread: g plays from the audio thread's next block; NULL is
   silence. The player owns g from here. Also reclaims. */
void funk_player_play(funk_player *p, funk_graph *g);

/* Host's thread: frees replaced graphs the audio thread has left. */
void funk_player_reclaim(funk_player *p);

/* The audio thread, only one: n frames of the current graph's output. */
void funk_player_render(funk_player *p, float *out, int n);

/* Once the audio thread has stopped calling funk_player_render. */
void funk_player_free(funk_player *p);

#endif
