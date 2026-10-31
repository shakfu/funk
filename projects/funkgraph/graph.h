/* graph.h -- runs a graph of kernels on an audio thread (docs/dev/audio.md, D).

   A kernel is a foreign export emitted by `funk emit-c --kernels`, with the
   signature funk_kernel: it writes n frames to out from its inputs, each n
   frames, and its state. A parameter, such as a frequency, is an input; a
   kernel may read one input's first frame only, once per call. A graph is
   built on the host's thread: each node is a kernel, a copy of its initial
   state, and inputs from earlier nodes, so a graph has no cycles. The last
   node added is the output, unless outputs are given, one per channel.
   Everything a node needs is allocated when it
   is added; rendering allocates nothing.

   A control is a node without a kernel: its output is a value, which
   events sent to its name change from their frame on, so a note's gate
   or frequency changes at its sample, not at a block's start.

   A player renders one graph at a time on one audio thread. funk_player_play
   replaces it from the host's thread at the next block; the old graph is
   freed by a later funk_player_reclaim, once the audio thread has finished
   a block without it and the block after, which may copy states from it. */
#ifndef FUNK_GRAPH_H
#define FUNK_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What every kernel may read. Fields are only ever added at the end. */
typedef struct {
  double rate; /* frames per second */
} funk_context;

typedef void (*funk_kernel)(void *state, float **in, float *out, int n, const funk_context *cx);

typedef struct funk_graph funk_graph;
typedef struct funk_player funk_player;

/* A graph rendering at most max_block frames per kernel call at rate
   frames per second; NULL if memory runs out. */
funk_graph *funk_graph_new(int max_block, double rate);

/* Adds a node: kernel k, size bytes of state copied from state (or zeroed
   if state is NULL), and nin inputs, each an earlier node's index or -1 for
   silence. The node's index, or -1 if an input is not an earlier node or
   memory runs out. Not after the graph is played. */
int funk_graph_add(funk_graph *g, funk_kernel k, const void *state, size_t size, const int *inputs, int nin);

/* Gives kernel node `node` zeroed memory for seconds of floats, plus 2
   frames. When played, the first 16 bytes of its state become the pointer
   and the frame count, an int64_t; its initial state should hold zeros there.
   A node that keeps its state across a swap keeps its memory, by swapping
   pointers on the audio thread, if the frame count is unchanged. 0, or -1 if
   node is a control, has memory or has under 16 bytes of state, or memory
   runs out. Not after the graph is played. */
int funk_graph_memory(funk_graph *g, int node, double seconds);

/* Adds a control, whose output starts at value; its index, or -1 if
   memory runs out. Events reach it once it is named. */
int funk_graph_control(funk_graph *g, float value);

/* Names node; 0, or -1 if the name is taken in g or node has one. A named
   node starts from the state of the node of its name in the graph the
   player rendered before, if that has the same kernel and initial state:
   an oscillator then keeps its phase across a swap, and a node whose
   initial state changed starts again. funk_player_play names the other
   kernel nodes by shape, their kernels' and inputs' without states, and
   their order among nodes of that shape: an oscillator keeps its phase
   when its frequency's constant changes or a filter is put around it.
   Not after the graph is played. */
int funk_graph_name(funk_graph *g, int node, const char *name);

/* Makes node the output of the graph's next channel; the channel, or -1
   if node is not in g or memory runs out. A graph without outputs has one:
   its last node. Not after the graph is played. */
int funk_graph_output(funk_graph *g, int node);

/* A graph that was never played. */
void funk_graph_free(funk_graph *g);

funk_player *funk_player_new(void);

/* Host's thread: g plays from the audio thread's next block; NULL is
   silence. The player owns g from here. Also reclaims. */
void funk_player_play(funk_player *p, funk_graph *g);

/* Host's thread: frees replaced graphs the audio thread has left. */
void funk_player_reclaim(funk_player *p);

/* Host's thread, in time order: from frame on, the control named name in
   the graph then playing outputs value. A frame already rendered counts as
   the next block's first; a name with no control there is ignored. False
   if the queue, 4096 events, is full. */
bool funk_player_send(funk_player *p, int64_t frame, const char *name, float value);

/* The audio thread, between renders, in time order: funk_player_send
   without the host's queue, for events made on the audio thread, such as
   a plugin's notes. Both queues' events take effect in frame order. */
bool funk_player_send_audio(funk_player *p, int64_t frame, const char *name, float value);

/* The host's thread or the audio thread, between renders: whether the
   graph now playing has a control named name. */
bool funk_player_has(funk_player *p, const char *name);

/* Any thread: the frames rendered so far, the next event's earliest frame. */
int64_t funk_player_frames(funk_player *p);

/* The audio thread, only one: n frames of the current graph's channels
   into out[0] to out[channels - 1]. Channel c plays the graph's output
   c modulo its outputs, so a mono graph plays on every channel. */
void funk_player_render_channels(funk_player *p, float *const *out, int channels, int n);

/* funk_player_render_channels with one channel: the graph's first output. */
void funk_player_render(funk_player *p, float *out, int n);

/* Once the audio thread has stopped calling funk_player_render. */
void funk_player_free(funk_player *p);

/* Notes on the voices of lib/dsp.fk's d.poly and d.polyOn: events to the
   controls "vK.freq" and "vK.gate", or "cC.vK.freq" and "cC.vK.gate" for
   MIDI channel C, velocity / 127 while held. A note on channel C plays
   channel C's voices if the graph now playing has "cC.v1.gate", else the
   shared ones. A note takes the free voice released longest ago, else the
   voice of the oldest note; a held or just-released voice's gate drops for
   a frame, so its envelope restarts. Events go out in time order, an
   earlier frame moved up to the last one sent. One thread uses a pool: the
   host's, or, made with audio_thread, the audio thread's. Calls but _new
   and _free allocate nothing; each returns false if the player's queue
   was full. */
typedef struct funk_voices funk_voices;

/* n voices, at most 64, shared and per channel; NULL if n is out of range
   or memory runs out. */
funk_voices *funk_voices_new(funk_player *p, int n, bool audio_thread);
void funk_voices_free(funk_voices *v);

/* A note on channel chan, 0 to 15, from frame on, velocity 1 to 127. It
   ends at frame off, or at funk_voices_off if off is INT64_MAX. Earlier
   notes' offs before on go out first. */
bool funk_voices_on(funk_voices *v, int64_t on, int64_t off, int chan, int note, int vel);

/* The oldest held voice of note on chan ends at frame. */
bool funk_voices_off(funk_voices *v, int64_t frame, int chan, int note);

/* The offs before frame, earliest first. */
bool funk_voices_until(funk_voices *v, int64_t frame);

/* Every held voice ends at frame. */
bool funk_voices_hush(funk_voices *v, int64_t frame);

#endif
