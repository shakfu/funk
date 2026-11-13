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
  double done; /* a kernel of an event sets it nonzero to end the event at
                  its block's end, as SuperCollider's doneAction 14 frees a
                  synth's group; of an orbit's effect, to pause it, as
                  doneAction 1; elsewhere it is ignored */
  double block; /* in the event layer, its blocks rendered: scsynth's mBufCounter */
  double *rgen; /* the player's random generator, scsynth's world RGen
                   (SC_RGen.h): s1, s2 and s3, each a uint32 in a double */
  double ctor; /* nonzero in a UGen's constructor call (funk_graph_ugen) */
  double freeself; /* a kernel of an event's stage sets it nonzero to free
                      the stage at its block's end, as doneAction 2 frees a
                      synth: from then on it is skipped and the chain passes
                      it unchanged; elsewhere it is ignored */
  const double *tables; /* scsynth's sine and cosecant tables (Samp.cpp),
                           8193 points each, float32s in doubles */
} funk_context;

/* The event layer: orbits, channels per orbit, and its block in frames. */
enum { FUNK_ORBITS = 16, FUNK_CHANNELS = 2, FUNK_EVENT_BLOCK = 64 };

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

/* Gives kernel node `node` read-only data, shared with other nodes and kept
   until the last graph reading it is freed (table.h). When played, the first
   24 bytes of its state become the data's pointer, its frames, an int64_t, and
   its frames per second, a double; its initial state should hold zeros there.
   A node that keeps its state across a swap keeps its position, but reads
   the new graph's data. 0, or -1 if node is a control, has memory or data or
   has under 24 bytes of state, or loading fails: funk_table_error. A
   file is read on the calling thread. Not after the graph is played. */
int funk_graph_table_file(funk_graph *g, int node, const char *path, int channel);
int funk_graph_table_values(funk_graph *g, int node, const double *values, int64_t frames);
int funk_graph_table_builtin(funk_graph *g, int node, const char *name, int channel); /* table.h */
const char *funk_table_error(void);

/* The index-th WAV file of dir/name, sorted by name, the index wrapping as
   SuperDirt's does: a Tidal sample such as "sn:3". "", with
   funk_table_error, if name is not one folder's name or the folder has no
   WAV file. A leading "~/" in dir is $HOME. The result lasts until the
   thread's next call. */
const char *funk_sample_path(const char *dir, const char *name, int index);

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
   In a template, a node named "fed:NAME" takes, at each block's end, the
   output of the node named "feed:NAME", as scsynth's LocalIn reads what
   LocalOut wrote in the block before. Not after the graph is played. */
int funk_graph_name(funk_graph *g, int node, const char *name);

/* Adds a bus: a node whose output is channel of orbit, 0 to FUNK_ORBITS - 1
   and 0 to FUNK_CHANNELS - 1, of the player's events. Its index, or -1 if
   out of range or memory runs out. A graph with no bus has every orbit
   added to its output instead. */
int funk_graph_bus(funk_graph *g, int orbit, int channel);

/* Adds an input of a template: channel, 0 to FUNK_CHANNELS - 1, of its
   event's chain, which the stages before it wrote; in an orbit's effect,
   up to 2 * FUNK_CHANNELS - 1, the dry bus, then the effect bus. In a graph that is
   played, silence. Its index, or -1. */
int funk_graph_in(funk_graph *g, int channel);

/* Marks kernel node `node` a UGen, constructed as scsynth constructs a
   synth's (Graph_FirstCalc): in a template, before its stage's first
   block, each UGen in order is called once for 1 frame with ctor set,
   reading its inputs' first frame: a control's value, the chain's, or an
   earlier UGen's constructor frame. In a played graph there is no such
   call. 0, or -1 if node is not a kernel node of g. Not after the graph
   is played. */
int funk_graph_ugen(funk_graph *g, int node);

/* Gives kernel node `node` k outputs, before any node reads it: a call of
   n frames writes output j at out + j * n, which a later node reads from
   its input, so one state can compute several channels. 0, or -1. */
int funk_graph_outputs(funk_graph *g, int node, int k);

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

/* Templates (docs/dev/dirt-sound.md): graphs that are not played but made
   anew for each event, from a fixed pool of slots, on the audio thread.
   An event's chain is one source, then the modules its parameters select,
   in the order added; each stage reads the chain through its inputs
   (funk_graph_in) and replaces it with its outputs. The chain renders in
   blocks of FUNK_EVENT_BLOCK frames into its orbit, from the start of the
   block that holds its frame, its output delayed to that frame, as
   scsynth's OffsetOut. An event ends when a kernel sets its context's
   done. */
typedef struct funk_templates funk_templates;

/* A set of templates, each stage of an event in a slot; NULL if slots is
   not positive or memory runs out. Templates whose sizes round up to one
   power of two share a size class, of slots slots, or fewer, at least
   one, if they would pass 8 MB, scsynth's default real-time memory; then
   classes with the fewest get more while all fit in 128 MB, the memSize
   SuperDirt.start sets. An
   event is dropped when a class it needs is full. */
funk_templates *funk_templates_new(int slots);

/* Adds g, which the set then owns, as the template name. "s.KEY" is a
   source, which an event names; "m.NAME" is a module, which an event
   with a nonzero parameter "~NAME" runs. "o.NAME" and "a.NAME" are an
   orbit's effects, as SuperDirt's GlobalDirtEffect: each orbit has one of
   each, in the order added, after the orbit's events in each block. Their
   inputs 0 and 1 read the orbit's dry bus, 2 and 3 its effect bus. With 4
   outputs, an effect replaces both; with 2, it is the monitor, and its
   outputs are the orbit's. A kernel that sets done pauses its effect. An
   event resumes an "o." effect that has one of its parameters, and an
   "a." effect always; it sets a parameter whose value differs from the
   last such event's, to 0 if the event lacks it, and its control
   "resumed" to 1 for one block. A new set keeps an effect's state, paused
   or not, if its graph and name are unchanged. g must have been made with a
   max_block of at least FUNK_EVENT_BLOCK, have no bus and not be played.
   0, or -1, freeing g, if not, if name is taken, or memory runs out. */
int funk_templates_add(funk_templates *t, const char *name, funk_graph *g);

/* Seeds the player's random generator from the next block, as scsynth's
   RandSeed does (RGen::init). A player is seeded from the time. */
void funk_player_seed(funk_player *p, uint32_t seed);

/* A set never given to a player. */
void funk_templates_free(funk_templates *t);

/* Host's thread: events from the next block use t; NULL ends new events.
   The player owns t from here. Events already playing keep their set,
   which is freed once they end. 0, or -1, freeing t, if memory runs out. */
int funk_player_templates(funk_player *p, funk_templates *t);

/* The host's thread or the audio thread, between renders: whether the
   player's set has the template name. */
bool funk_player_has_template(funk_player *p, const char *name);

/* Host's thread, in time order: an event from frame of the source
   template source, with parameters names[i] = values[i]. Each sets the
   control of its name in every stage; "orbit" picks the orbit, 0 by
   default; "cut" a cut group, as SuperDirt's; and each also sets the
   played graph's control "oORBIT.NAME", a global effect's parameter,
   from frame on. False if the queue, 256 events, is full or n is over 48. */
bool funk_player_event(funk_player *p, int64_t frame, const char *source, int n, const char *const *names, const float *values);

/* Host's thread: funk_player_event in parts, for callers without arrays
   of strings, such as funk: begin, each parameter, then end, which sends
   it. False if a part does not fit, or, from end, if the queue is full. */
bool funk_player_event_begin(funk_player *p, int64_t frame, const char *source);
bool funk_player_event_param(funk_player *p, const char *name, float value);
bool funk_player_event_end(funk_player *p);

/* The audio thread, between renders, in time order: funk_player_event. */
bool funk_player_event_audio(funk_player *p, int64_t frame, const char *source, int n, const char *const *names,
                             const float *values);

/* Any thread: events playing, and those dropped because the pool or the
   list of events was full, or their source was missing, and those that
   came after their block had rendered, which start at the next. */
void funk_player_event_counts(funk_player *p, int *live, uint64_t *dropped, uint64_t *late);

/* Notes on the voices of lib/dsp.fk's d.poly and d.polyOn: events to the
   controls "vK.freq" and "vK.gate", or "cC.vK.freq" and "cC.vK.gate" for
   MIDI channel C, velocity / 127 while held. Each note N also sets "kN.gate",
   d.key's, on any channel, while any of its notes is held; it drops for a
   frame to rise again. A note on channel C plays
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

/* funk_voices_on, and before the gate rises, each of the n names[i] to
   values[i] on the note's voice: control "vK.name", or "cC.vK.name", if
   the graph has it. A control pattern's parameters (lib/control.fk). */
bool funk_voices_on_with(funk_voices *v, int64_t on, int64_t off, int chan, int note, int vel, int n,
                         const char *const *names, const float *values);

/* A sample's trigger, as lib/dsp.fk's d.kit reads it: from frame, each of
   the n names[i] to values[i] on control "prefix.V.name", then
   "prefix.V.gate" to 1 for one frame. V is prefix's next voice, 1 up to the
   last the graph has, in turn; a graph with none takes "prefix.name".
   Controls the graph lacks are skipped. A nonzero "cut" first stops, by
   "prefix.V.stop", each voice last played in the same orbit with a cut of
   that size: of any sample if the cut is positive, of this one if negative,
   as SuperDirt's cut groups. Earlier offs go out first, as for a note.
   If the player's templates have prefix, the trigger is an event of that
   source instead (funk_player_event), with the same parameters. */
bool funk_voices_trigger(funk_voices *v, int64_t frame, const char *prefix, int n, const char *const *names,
                         const float *values);

/* The oldest held voice of note on chan ends at frame. */
bool funk_voices_off(funk_voices *v, int64_t frame, int chan, int note);

/* The offs before frame, earliest first. */
bool funk_voices_until(funk_voices *v, int64_t frame);

/* Every held voice ends at frame. */
bool funk_voices_hush(funk_voices *v, int64_t frame);

#endif
