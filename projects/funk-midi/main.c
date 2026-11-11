/* funk-midi -- plays a funk pattern to a MIDI port, live (docs/dev/patterns.md).

   A REPL over libfunk.h, with `pattern as p` and `midi as m` imported, and
   control and stream unqualified. Tidal's slots, `d1 $ ...` and `p "name"
   $ ...`, play control patterns, each replaced alone (stream.c,
   docs/dev/stream.md); `pat` of Controls is d1's. Whenever input defines
   `pat`, a Pattern Int (lib/midi.fk gives the values), it plays from the
   next window, beside the slots. A clock runs from start-up, so patterns
   stay on one cycle grid across redefinitions. With --dirt, the slots go
   to SuperDirt instead, one OSC bundle per onset (dirt.c,
   docs/dev/dirt.md).

   Threads: funk's runs the REPL input and, every 10 ms window, calls
   `step` through funk_call for the events 50 ms ahead; a sender thread
   sends them at their times, so funk's work never delays a message; an
   input thread reads lines. --no-sleep runs on a virtual clock instead,
   printing each message: for tests.

   With --audio, `graph as g` and `dsp as d` are imported too, and input
   that defines `sig`, a g.Sig, plays it from the next audio block
   (docs/dev/audio.md, D). The host owns the player and the kernel
   library; funk builds each graph through `playsig`. The audio thread
   renders through hosts/graph alone, never funk. A graph of g.Channels
   plays one channel per output channel, a mono one on all. On the virtual
   clock, the peak over both channels is printed every 250 ms instead. */
#define _DEFAULT_SOURCE
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#include <dlfcn.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "controls.h"
#include "dirt.h"
#include "graph.h"
#include "libfunk.h"
#include "stream.h"

#ifdef HAVE_LIBREMIDI
#include <libremidi/libremidi-c.h>
#endif
#ifdef HAVE_MINIAUDIO
#include "miniaudio.h"
#endif

#ifndef FUNK_LIB_DIR
#define FUNK_LIB_DIR "lib"
#endif
#ifndef FUNK_DSP_LIB
#define FUNK_DSP_LIB "funkdsp.so"
#endif
#ifndef FUNK_GRAPH_INCLUDE
#define FUNK_GRAPH_INCLUDE "hosts/graph"
#endif

enum { WINDOW_US = 10000, CAP = 512, CYCLE_TICKS = 720720, DIRT_BYTES = 65536 };
/* how far ahead step runs: 50 ms; with --dirt, Tidal's 0.1 s (docs/dev/dirt.md) */
static int64_t lookahead_us = 50000;
/* for pat, a Pattern Int */
#define STEP "step buf ctl nctl cs ot a b = p.writeEventsWith (plusPtr nullPtr buf) 512 (p._shift (negate ot) pat) (p.readControls (plusPtr nullPtr ctl) nctl) cs a b"
/* for the slots, d1 and the rest (lib/stream.fk): time, MIDI bytes and note length (control.fk) */
#define SLOTS "slots buf ctl nctl cs ot a b tb = case p.readControls (plusPtr nullPtr ctl) nctl of\n cl -> case resolveWindow cl (p.windowOnsets cl (p._shift (negate ot) (playStack ())) cs a b) of\n  es -> do\n    writeTempo (plusPtr nullPtr tb) cs es\n    writeControlEvents (plusPtr nullPtr buf) 512 es cs 1000000"
/* for --dirt: the onsets with an `s`, for SuperDirt (writeDirtWith, lib/control.fk) */
#define SLOTS_DIRT "slots buf ctl nctl cs ot a b tb = case p.readControls (plusPtr nullPtr ctl) nctl of\n cl -> case resolveWindow cl (p.windowOnsets cl (p._shift (negate ot) (playStack ())) cs a b) of\n  es -> do\n    writeTempo (plusPtr nullPtr tb) cs es\n    writeDirtEvents (plusPtr nullPtr buf) 65536 cs ot es"
/* after a window fails, as Tidal's doTick: each slot plays its previous pattern */
#define REVERT "revertSlots x = do\n  setPreviousPatternOrSilence ()\n  (x :: Int)"
#define CONTROL_IMPORT "import control (..)"
#define STREAM_IMPORT "import stream (..)"

static bool virtual_time, print_sink, audio, dirt;
static int dirt_fd = -1;          /* --dirt's socket */
static volatile int dirt_stop, dirt_connected; /* the handshake's */
static int64_t unix_at_t0;        /* the Unix time, in us, of now_us() 0: for OSC time tags */
static volatile sig_atomic_t interrupted;

/* ------------------------------------------------------------------ time */

static int64_t vnow, t0;

static int64_t mono_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* microseconds since start-up */
static int64_t now_us(void) { return virtual_time ? vnow : mono_us() - t0; }

/* an absolute CLOCK_REALTIME time d microseconds from now, for timed waits */
static struct timespec after_us(int64_t d) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  int64_t ns = ts.tv_nsec + (d < 0 ? 0 : d) * 1000;
  ts.tv_sec += ns / 1000000000;
  ts.tv_nsec = ns % 1000000000;
  return ts;
}

/* ------------------------------------------------------------------ output */

/* MIDI CC in, for controls' p.midiCC (docs/dev/controls-in.md): the input's
   thread and :cc set a value, the thread that calls step takes them */
static pthread_mutex_t cc_mu = PTHREAD_MUTEX_INITIALIZER;
static short cc_in[16][128]; /* value + 1; 0: none since last taken */

static void cc_set(int channel, int num, int value) {
  pthread_mutex_lock(&cc_mu);
  cc_in[channel & 15][num & 127] = (short)((value & 127) + 1);
  pthread_mutex_unlock(&cc_mu);
}

#ifdef HAVE_LIBREMIDI
static libremidi_midi_out_handle *out;
static libremidi_midi_out_port *ports[64];
static int nports;

static void on_port(void *ctx, const libremidi_midi_out_port *port) {
  (void)ctx;
  if (nports < 64) libremidi_midi_out_port_clone(port, &ports[nports++]);
}

static void find_ports(void) {
  libremidi_observer_configuration oc;
  libremidi_api_configuration ac;
  libremidi_midi_observer_handle *obs;
  libremidi_midi_observer_configuration_init(&oc);
  oc.track_hardware = oc.track_virtual = oc.track_any = true;
  libremidi_midi_api_configuration_init(&ac);
  ac.configuration_type = Observer;
  if (libremidi_midi_observer_new(&oc, &ac, &obs) != 0) return;
  libremidi_midi_observer_enumerate_output_ports(obs, NULL, on_port);
  libremidi_midi_observer_free(obs);
}

static libremidi_midi_in_handle *in;
static libremidi_midi_in_port *in_ports[64];
static int nin_ports;

static void on_in_port(void *ctx, const libremidi_midi_in_port *port) {
  (void)ctx;
  if (nin_ports < 64) libremidi_midi_in_port_clone(port, &in_ports[nin_ports++]);
}

static void on_in(void *ctx, libremidi_timestamp ts, const libremidi_midi1_symbol *m, size_t len) {
  (void)ctx, (void)ts;
  if (len == 3 && (m[0] & 0xf0) == 0xb0) cc_set(m[0] & 15, m[1], m[2]);
}

static void find_in_ports(void) {
  libremidi_observer_configuration oc;
  libremidi_api_configuration ac;
  libremidi_midi_observer_handle *obs;
  libremidi_midi_observer_configuration_init(&oc);
  oc.track_hardware = oc.track_virtual = oc.track_any = true;
  libremidi_midi_api_configuration_init(&ac);
  ac.configuration_type = Observer;
  if (libremidi_midi_observer_new(&oc, &ac, &obs) != 0) return;
  libremidi_midi_observer_enumerate_input_ports(obs, NULL, on_in_port);
  libremidi_midi_observer_free(obs);
}

/* port is an index, or -1 for a virtual input named name */
static bool open_in(int port, const char *name) {
  libremidi_midi_configuration mc;
  libremidi_api_configuration ac;
  libremidi_midi_configuration_init(&mc);
  libremidi_midi_api_configuration_init(&ac);
  ac.configuration_type = Input;
  mc.version = MIDI1;
  mc.on_midi1_message.context = NULL;
  mc.on_midi1_message.callback = on_in;
  if (port < 0) {
    mc.virtual_port = true;
    mc.port_name = name;
  } else {
    find_in_ports();
    if (port >= nin_ports) {
      fprintf(stderr, "funk-midi: no input port %d; --list shows them\n", port);
      return false;
    }
    mc.in_port = in_ports[port];
  }
  if (libremidi_midi_in_new(&mc, &ac, &in) != 0) {
    fputs("funk-midi: cannot open the MIDI input\n", stderr);
    return false;
  }
  return true;
}

static void list_ports(void) {
  find_in_ports();
  for (int i = 0; i < nin_ports; i++) {
    const char *name;
    size_t len;
    if (libremidi_midi_in_port_name(in_ports[i], &name, &len) == 0) printf("in %d: %.*s\n", i, (int)len, name);
  }
  find_ports();
  for (int i = 0; i < nports; i++) {
    const char *name;
    size_t len;
    if (libremidi_midi_out_port_name(ports[i], &name, &len) == 0) printf("%d: %.*s\n", i, (int)len, name);
  }
}

/* port is an index, or -1 for a virtual port named name */
static bool open_out(int port, const char *name) {
  libremidi_midi_configuration mc;
  libremidi_api_configuration ac;
  libremidi_midi_configuration_init(&mc);
  libremidi_midi_api_configuration_init(&ac);
  ac.configuration_type = Output;
  mc.version = MIDI1;
  if (port < 0) {
    mc.virtual_port = true;
    mc.port_name = name;
  } else {
    find_ports();
    if (port >= nports) {
      fprintf(stderr, "funk-midi: no output port %d; --list shows them\n", port);
      return false;
    }
    mc.out_port = ports[port];
  }
  if (libremidi_midi_out_new(&mc, &ac, &out) != 0) {
    fputs("funk-midi: cannot open the MIDI output\n", stderr);
    return false;
  }
  return true;
}
#endif

/* t: the message's time. Printed in virtual time; in real time, the time it is sent, which shows jitter. */
static void send_msg(int64_t t, const unsigned char *m, size_t size) {
  if (print_sink) {
    if (!virtual_time) t = now_us();
    static const char *const kinds[] = {"off", "on ", "poly", "cc ", "pc ", "touch", "bend"};
    if (m[0] >= 0xf0) printf("%9.3f sys %d %d %d\n", (double)t / 1000, m[0], size > 1 ? m[1] : 0, size > 2 ? m[2] : 0);
    else printf("%9.3f %s ch%d %d %d\n", (double)t / 1000, kinds[(m[0] >> 4) - 8], (m[0] & 15) + 1, m[1], size > 2 ? m[2] : 0);
    fflush(stdout);
    return;
  }
#ifdef HAVE_LIBREMIDI
  libremidi_midi_out_send_message(out, m, size);
#endif
}

/* ------------------------------------------------- the queue of messages */

typedef struct { int64_t t; uint64_t seq; unsigned char m[3], size; } Msg;
static Msg *heap;
static size_t nheap, capheap;
static uint64_t nseq;
static bool quitting;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queued = PTHREAD_COND_INITIALIZER;

/* earlier first; at one time, note-offs before note-ons, so a note that
   ends where the next begins is released first */
static bool before(const Msg *a, const Msg *b) {
  if (a->t != b->t) return a->t < b->t;
  bool aon = (a->m[0] & 0xf0) == 0x90, bon = (b->m[0] & 0xf0) == 0x90;
  if (aon != bon) return bon;
  return a->seq < b->seq;
}

static void heap_push(Msg m) {
  if (nheap == capheap) {
    capheap = capheap ? 2 * capheap : 256;
    heap = realloc(heap, capheap * sizeof *heap);
    if (!heap) { fputs("funk-midi: out of memory\n", stderr); exit(1); }
  }
  size_t i = nheap++;
  for (; i && before(&m, &heap[(i - 1) / 2]); i = (i - 1) / 2) heap[i] = heap[(i - 1) / 2];
  heap[i] = m;
}

static Msg heap_pop(void) {
  Msg top = heap[0], last = heap[--nheap];
  size_t i = 0;
  for (;;) {
    size_t c = 2 * i + 1;
    if (c >= nheap) break;
    if (c + 1 < nheap && before(&heap[c + 1], &heap[c])) c++;
    if (!before(&heap[c], &last)) break;
    heap[i] = heap[c];
    i = c;
  }
  if (nheap) heap[i] = last;
  return top;
}

static void enqueue_sized(int64_t t, unsigned char status, unsigned char d1, unsigned char d2, unsigned char size) {
  pthread_mutex_lock(&mu);
  heap_push((Msg){t, nseq++, {status, d1, d2}, size});
  pthread_cond_signal(&queued);
  pthread_mutex_unlock(&mu);
}

static void enqueue(int64_t t, unsigned char status, unsigned char d1, unsigned char d2) { enqueue_sized(t, status, d1, d2, 3); }

/* Drops the queued note-ons and moves the note-offs to now: every note
   sounding or due stops. */
static void hush(void) {
  pthread_mutex_lock(&mu);
  size_t n = nheap;
  if (!n) { /* memcpy from a NULL heap is undefined even for 0 bytes, and malloc(0) may return NULL */
    pthread_mutex_unlock(&mu);
    return;
  }
  Msg *all = malloc(n * sizeof *all);
  if (!all) { fputs("funk-midi: out of memory\n", stderr); exit(1); }
  memcpy(all, heap, n * sizeof *all);
  nheap = 0;
  int64_t now = now_us();
  for (size_t i = 0; i < n; i++)
    if ((all[i].m[0] & 0xf0) == 0x80) heap_push((Msg){now, all[i].seq, {all[i].m[0], all[i].m[1], all[i].m[2]}, 3});
  free(all);
  pthread_cond_signal(&queued);
  pthread_mutex_unlock(&mu);
}

/* virtual time: sends what is due before t */
static void flush_until(int64_t t) {
  while (nheap && heap[0].t < t) {
    Msg m = heap_pop();
    send_msg(m.t, m.m, m.size);
  }
}

#ifdef __APPLE__
/* A thread of default policy wakes from a timed wait up to 8 ms late; as a
   time-constraint (real-time) thread, under 0.5 ms (docs/dev/patterns.md). */
static void realtime(void) {
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  double ms = 1e6 * tb.denom / tb.numer; /* mach time units per ms */
  thread_time_constraint_policy_data_t p = {(uint32_t)ms, (uint32_t)(0.1 * ms), (uint32_t)ms, 1};
  thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                    THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}
#endif

static funk *session; /* this program has one */

static void *sender(void *arg) {
  (void)arg;
#ifdef __APPLE__
  realtime();
#endif
  pthread_mutex_lock(&mu);
  for (;;) {
    while (!nheap && !quitting) pthread_cond_wait(&queued, &mu);
    if (!nheap) break;
    int64_t d = heap[0].t - now_us();
    if (d > 0) {
      struct timespec ts = after_us(d);
      pthread_cond_timedwait(&queued, &mu, &ts);
      continue;
    }
    Msg m = heap_pop();
    pthread_mutex_unlock(&mu);
    send_msg(m.t, m.m, m.size);
    pthread_mutex_lock(&mu);
  }
  pthread_mutex_unlock(&mu);
  return NULL;
}

/* --------------------------------------------------------- the pattern */

static long buf[3 * CAP]; /* start, duration, value */
static long dbuf[DIRT_BYTES / sizeof(long)]; /* --dirt's events */

static bool playing = true, have_step; /* have_step: pat, a Pattern Int, plays */
static void osc_command(void *ctx, const char *addr, int nargs, int kind, const char *arg);
static void retempo_events(int64_t base);

/* A control record's tags that are not MIDI (writeControls, lib/control.fk). */
enum { TAG_NAME = 0xf4, TAG_PARAM = 0xf5, TAG_TRIGGER = 0xfd };
static bool internal_tag(long m) { return (m & 255) == TAG_NAME || (m & 255) == TAG_PARAM || (m & 255) == TAG_TRIGGER; }
/* the tempo: Tidal's default, 0.575 cycles a second (Clock.hs, defaultCps);
   cs, microseconds per cycle, its grid, to the nearest */
static double cps = 0.575;
static int64_t cs;
static long tbuf[1 + 2 * 64]; /* the window's tempo changes (writeTempo, lib/stream.fk) */
static int64_t origin_us, origin_tick, win; /* tick origin_tick sounds at origin_us; win: the next window */
/* step's controls (docs/dev/controls-in.md): _cps, and _t_pattern, the cycle
   pat last changed at; each slot has its own (lib/stream.fk) */
static funk_controls controls;
static int osc_fds[8], nosc; /* controls' p.osc ports */
static bool cc_on;          /* controls has p.midiCC */

/* the CCs set since the last call, into controls if p.midiCC is on */
static void cc_take(void) {
  pthread_mutex_lock(&cc_mu);
  for (int c = 0; c < 16; c++)
    for (int k = 0; k < 128; k++)
      if (cc_in[c][k]) {
        if (cc_on) funk_control_cc(&controls, c, k, cc_in[c][k] - 1);
        cc_in[c][k] = 0;
      }
  pthread_mutex_unlock(&cc_mu);
}

static void voice_notes(long n);
static void voice_controls(long n);
static void voice_sync(void);
static void voice_release_until(int64_t t);

/* --dirt: each event of dbuf as a bundle at its onset, plus its nudge, sent now */
static void send_dirt(long n) {
  const unsigned char *p = (const unsigned char *)dbuf, *end = p + sizeof dbuf;
  static unsigned char out[DIRT_BYTES];
  long sent = 0;
  dirt_event e;
  for (; dirt_next(p, end, &e); p = e.next, sent++) {
    int64_t at = origin_us + e.start + (int64_t)(dirt_float(&e, "nudge", 0) * 1e6);
    size_t size = dirt_bundle(&e, dirt_ntp(unix_at_t0 + at), cps, (double)e.dur / 1e6, out, sizeof out);
    if (!size) fputs("funk-midi: an event too large for one datagram\n", stderr);
    else if (print_sink) dirt_print(stdout, out, size, 0);
    else if (send(dirt_fd, out, size, 0) < 0 && dirt_connected) perror("funk-midi: sending to SuperDirt"); /* before, the handshake reports */
  }
  if (print_sink) fflush(stdout);
  if (sent < n) fprintf(stderr, "funk-midi: %ld events in a window; sent %ld\n", n, sent);
}

/* Calls `step` for every window that starts within the lookahead. */
static void schedule(void) {
  while (win <= now_us() + lookahead_us) {
    for (int i = 0; i < nosc; i++) funk_osc_poll_commands(osc_fds[i], &controls, osc_command, NULL);
    bool slots = funk_stream_count() > 0 || funk_stream_firsts() > 0;
    if (playing && (have_step || slots)) {
      cc_take();
      funk_control_float(&controls, "_cps", cps);
      if (audio) voice_sync();
      long args[8] = {dirt ? (long)dbuf : (long)buf, (long)controls.e, controls.n, (long)cs, (long)origin_tick, (long)(win - origin_us),
                      (long)(win + WINDOW_US - origin_us), (long)tbuf}, n, r;
      int64_t base = origin_us; /* the units' zero, until the window's tempo changes at its end */
      bool tempo = false;
      if (slots && !funk_call(session, "slots", 8, args, &n)) {
        fputs("funk-midi: a window failed; each slot returns to its previous pattern\n", stderr); /* the error is reported */
        if (!funk_call(session, "revertSlots", 1, args, &r)) fputs("funk-midi: the slots could not return to their previous patterns\n", stderr);
      } else if (slots && dirt) {
        send_dirt(n);
        tempo = true;
      } else if (slots) {
        tempo = true;
        if (n > CAP) fprintf(stderr, "funk-midi: %ld events in a window; played %d\n", n, CAP);
        for (long i = 0; i < n && i < CAP; i++) {
          int64_t at = origin_us + buf[3 * i];
          long m = buf[3 * i + 1], length = buf[3 * i + 2];
          unsigned char st = m & 255, d1 = (m >> 8) & 255, d2 = (m >> 16) & 255, size = (m >> 24) & 3;
          if (internal_tag(m)) continue;
          enqueue_sized(at, st, d1, d2, size);
          if ((st & 0xf0) == 0x90 && length >= 0) enqueue(at + (length > 0 ? length : 1), (unsigned char)(0x80 | (st & 15)), d1, 0);
        }
        if (audio) voice_controls(n < CAP ? n : CAP);
      }
      args[0] = (long)buf;
      if (have_step && !funk_call(session, "step", 7, args, &n)) {
        have_step = false; /* a runtime error, reported: silence until pat is defined again */
        fputs("funk-midi: pat failed; stopped until it is defined again\n", stderr);
      } else if (have_step) {
        if (n > CAP) fprintf(stderr, "funk-midi: %ld events in a window; played %d\n", n, CAP);
        for (long i = 0; i < n && i < CAP; i++) {
          int64_t at = origin_us + buf[3 * i], dur = buf[3 * i + 1] > 0 ? buf[3 * i + 1] : 1;
          long v = buf[3 * i + 2];
          unsigned char note = v & 127, vel = (v >> 7) & 127, ch = (v >> 14) & 15;
          enqueue(at, (unsigned char)(0x90 | ch), note, vel ? vel : 100);
          enqueue(at + dur, (unsigned char)(0x80 | ch), note, 0);
        }
        if (audio) voice_notes(n < CAP ? n : CAP);
      }
      if (tempo) retempo_events(base);
    }
    win += WINDOW_US;
    if (audio) voice_release_until(win);
  }
}

/* -------------------------------------------------------------- audio */

enum { MAX_BLOCK = 512, REPORT = 12000 }; /* frames: 250 ms at 48 kHz on the virtual clock */
static funk_player *player;
static double rate = 48000;
static bool have_sig;
static int64_t rendered; /* frames, on the virtual clock */
static float peak;
static const char *playsig_src; /* compiled again after sig changes, so it names the new sig */

static bool eval(const char *src);

/* sig, or silence when stopped, from the next block. A sig that fails,
   such as one naming an unknown kernel, leaves the last graph playing. */
static void play_sig(void) {
  long r, arg = 0;
  if (!audio || !have_sig) return;
  if (!funk_call(session, playing ? "playsig" : "hushsig", 1, &arg, &r)) {
    have_sig = false;
    fputs("funk-midi: sig failed; the last graph plays on until sig is defined again\n", stderr);
  }
}

/* The virtual clock's audio, up to time t, with its peak over both
   channels every REPORT frames. */
static void render_until(int64_t t) {
  float l[MAX_BLOCK], r[MAX_BLOCK], *out[2] = {l, r};
  for (int64_t want = t * (int64_t)rate / 1000000; rendered < want;) {
    int64_t n = want - rendered, edge = (rendered / REPORT + 1) * REPORT;
    if (n > MAX_BLOCK) n = MAX_BLOCK;
    if (rendered + n > edge) n = edge - rendered;
    funk_player_render_channels(player, out, 2, (int)n);
    for (int64_t i = 0; i < n; i++) peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(r[i])));
    rendered += n;
    if (rendered % REPORT == 0) {
      printf("%9.3f audio peak %ld\n", (double)rendered * 1000 / rate, lroundf(peak * 10000));
      peak = 0;
    }
  }
}

/* ------------------------------------------------------------- voices */

/* pat's notes on d.poly's voices (funk_voices, projects/funkgraph). */
enum { MAX_VOICES = 64 };
static int nvoices = 8;
static funk_voices *voices;
static double frame_off; /* real time: audio frames ahead of the clock, smoothed */

static int64_t frame_at(int64_t t) {
  return virtual_time ? t * (int64_t)rate / 1000000 : (int64_t)floor((double)t * rate / 1e6 + frame_off);
}

/* The audio thread renders by its own clock: frame_off follows it, by
   1/64 of the error each window, so block-sized jumps in the count average out. */
static void voice_sync(void) {
  static bool synced;
  if (virtual_time) return;
  double e = (double)funk_player_frames(player) - ((double)now_us() * rate / 1e6 + frame_off);
  frame_off += synced ? e / 64 : e;
  synced = true;
}

static void voice_check(bool ok) {
  if (!ok) fputs("funk-midi: audio event queue full\n", stderr);
}

static void voice_release_until(int64_t t) { voice_check(funk_voices_until(voices, frame_at(t))); }

static int by_start(const void *a, const void *b) {
  long x = *(const long *)a, y = *(const long *)b;
  return (x > y) - (x < y);
}

/* A control pattern's records, in order: a name's chunks, then a
   parameter with that name; a note-on takes the parameters before it, for
   its voice, and a trigger takes them, and the name before it as its key,
   for d.kit's sample. The records come in time order, so they are not sorted. */
static void voice_controls(long n) {
  enum { MAX = 32, NAME = 64 };
  char name[NAME], names[MAX][NAME];
  const char *np[MAX];
  float values[MAX];
  size_t nl = 0;
  int k = 0;
  for (long i = 0; i < n; i++) {
    int64_t at = origin_us + buf[3 * i];
    long m = buf[3 * i + 1], length = buf[3 * i + 2];
    if ((m & 255) == TAG_NAME) {
      for (int b = 0; b < 8 && nl + 1 < NAME && ((unsigned long)length >> (8 * b) & 255); b++) name[nl++] = (char)((unsigned long)length >> (8 * b) & 255);
    } else if ((m & 255) == TAG_PARAM) {
      double x;
      memcpy(&x, &length, sizeof x);
      if (k < MAX) {
        memcpy(names[k], name, nl), names[k][nl] = 0;
        np[k] = names[k], values[k++] = (float)x;
      }
      nl = 0;
    } else if ((m & 255) == TAG_TRIGGER) {
      char prefix[NAME + 2];
      snprintf(prefix, sizeof prefix, "s.%.*s", (int)nl, name);
      voice_check(funk_voices_trigger(voices, frame_at(at), prefix, k, np, values));
      k = 0, nl = 0;
    } else if ((m & 0xf0) == 0x90 && length >= 0) {
      voice_check(funk_voices_on_with(voices, frame_at(at), frame_at(at + (length > 0 ? length : 1)), (int)(m & 15), (int)((m >> 8) & 127),
                                      (int)((m >> 16) & 127), k, np, values));
      k = 0;
    }
  }
}

/* the window's n events of pat in buf */
static void voice_notes(long n) {
  qsort(buf, (size_t)n, 3 * sizeof *buf, by_start);
  for (long i = 0; i < n; i++) {
    int64_t at = origin_us + buf[3 * i], dur = buf[3 * i + 1] > 0 ? buf[3 * i + 1] : 1;
    long v = buf[3 * i + 2], vel = (v >> 7) & 127;
    voice_check(funk_voices_on(voices, frame_at(at), frame_at(at + dur), (int)((v >> 14) & 15), (int)(v & 127), vel ? (int)vel : 100));
  }
}

/* :stop: every held voice's gate falls now */
static void voice_hush(void) { voice_check(funk_voices_hush(voices, frame_at(now_us()))); }

#ifdef HAVE_MINIAUDIO
static ma_device device;

static void on_audio(ma_device *d, void *out, const void *in, ma_uint32 n) {
  (void)in;
  enum { CH = 8 };
  static float buf[CH][MAX_BLOCK];
  float *chans[CH], *o = out;
  ma_uint32 ch = d->playback.channels < CH ? d->playback.channels : CH;
  for (ma_uint32 c = 0; c < ch; c++) chans[c] = buf[c];
  for (ma_uint32 done = 0; done < n;) {
    ma_uint32 m = n - done < MAX_BLOCK ? n - done : MAX_BLOCK;
    funk_player_render_channels(player, chans, (int)ch, (int)m);
    for (ma_uint32 i = 0; i < m; i++)
      for (ma_uint32 c = 0; c < d->playback.channels; c++) o[(done + i) * d->playback.channels + c] = c < ch ? buf[c][i] : 0;
    done += m;
  }
}
#endif

/* The player, the kernels and `playsig`; the device starts in real time. */
static bool start_audio(const char *kernels_path) {
  void *kernels = dlopen(kernels_path, RTLD_NOW | RTLD_LOCAL);
  if (!kernels) {
    fprintf(stderr, "funk-midi: cannot load the kernels: %s\n", dlerror());
    return false;
  }
#ifdef HAVE_MINIAUDIO
  if (!virtual_time) {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 48000;
    cfg.dataCallback = on_audio;
    if (ma_device_init(NULL, &cfg, &device) != MA_SUCCESS) {
      fputs("funk-midi: cannot open the audio device\n", stderr);
      return false;
    }
    rate = device.sampleRate;
  }
#endif
  if (!(player = funk_player_new())) return false;
  char *src;
  if (asprintf(&src, "audioPlayer = g.Player (plusPtr nullPtr %ld) [plusPtr nullPtr %ld] %.17g %d", (long)player, (long)kernels, rate,
               MAX_BLOCK) < 0)
    return false;
  bool ok = eval(src) && eval("hushsig :: Int ->> Int\nhushsig u = do { g.play audioPlayer g.Silence ; 0 }");
  free(src);
  if (!ok || asprintf(&src,
                       "voices :: (g.Sig -> g.Sig -> g.Sig) -> g.Sig\nvoices f = d.poly %d f\nvoicesIx :: (Int -> g.Sig -> g.Sig -> g.Sig) -> g.Sig\nvoicesIx f = d.polyIx %d f\n"
                       "voicesWith :: (g.Sig -> g.Sig -> (String -> Float -> g.Sig) -> g.Sig) -> g.Sig\nvoicesWith f = d.polyWith %d f\n"
                       "voicesOn :: Int -> (g.Sig -> g.Sig -> g.Sig) -> g.Sig\nvoicesOn ch f = d.polyOn ch %d f",
                       nvoices, nvoices, nvoices, nvoices) < 0) return false;
  ok = eval(src);
  free(src);
  if (!ok || !(voices = funk_voices_new(player, nvoices, false))) return false;
  playsig_src = "playsig :: Int ->> Int\nplaysig u = do { g.play audioPlayer sig ; 0 }";
#ifdef HAVE_MINIAUDIO
  if (!virtual_time && ma_device_start(&device) != MA_SUCCESS) {
    fputs("funk-midi: cannot start the audio device\n", stderr);
    return false;
  }
#endif
  return true;
}

static void stop_audio(void) {
#ifdef HAVE_MINIAUDIO
  if (!virtual_time) ma_device_uninit(&device);
#endif
  funk_voices_free(voices);
  funk_player_free(player);
}

/* Plays until time t. */
static void advance_until(int64_t t) {
  while (now_us() < t && !interrupted) {
    schedule();
    int64_t next = win - lookahead_us; /* above now: schedule ran */
    if (next > t) next = t;
    if (virtual_time) {
      flush_until(next);
      if (audio) render_until(next);
      vnow = next;
    } else {
      int64_t d = next - now_us();
      if (d > 0) usleep((useconds_t)d);
    }
  }
}

/* cycles per second from time `at`, so the beat stays */
static void retempo(int64_t at, double new_cps) {
  origin_tick += (at - origin_us) * CYCLE_TICKS / cs;
  origin_us = at;
  cps = new_cps;
  cs = llround(1e6 / new_cps);
}

static void set_cycle(int64_t us) { retempo(win, 1e6 / (double)us); } /* from the next unscheduled window */

/* Link's tempo range, 20 to 999 bpm, at 4 beats a cycle */
static double clamp_cps(double c) { return c < 20.0 / 240 ? 20.0 / 240 : c > 999.0 / 240 ? 999.0 / 240 : c; }

/* The window's events' `cps`, each from its onset, as Tidal's processCps. */
static void retempo_events(int64_t base) {
  for (long i = 0; i < tbuf[0] && i < 64; i++) {
    double c;
    memcpy(&c, &tbuf[2 + 2 * i], sizeof c);
    if (c > 0) retempo(base + tbuf[1 + 2 * i], clamp_cps(c));
  }
}

double funk_stream_cps(void) { return cps; }
void funk_stream_set_cps(double c) { if (c > 0) retempo(win, clamp_cps(c)); }
double funk_stream_cycle_now(void) { return ((double)origin_tick + (double)(now_us() - origin_us) * CYCLE_TICKS / (double)cs) / CYCLE_TICKS; }
void funk_stream_set_cycle(double c) { /* Tidal's setCycle: cycle c starts at the next window */
  origin_us = win;
  origin_tick = llround(c * CYCLE_TICKS);
}

/* -------------------------------------------------------------- input */

static bool eval(const char *src) {
  char *shown;
  bool ok = funk_eval(session, src, "<input>", &shown);
  if (shown) puts(shown);
  free(shown);
  return ok;
}

/* Tidal's listener's playback commands (Stream/Listen.hs): an ID is a
   string or an int; others are reported, as Tidal's are. */
static bool eval(const char *src);

static void osc_command(void *ctx, const char *addr, int nargs, int kind, const char *arg) {
  (void)ctx;
  static const char *const by_id[][2] = {{"/mute", "mute"}, {"/unmute", "unmute"}, {"/solo", "solo"}, {"/unsolo", "unsolo"}, {"/silence", "streamSilence"}};
  static const char *const plain[][2] = {{"/muteAll", "muteAll"}, {"/unmuteAll", "unmuteAll"}, {"/unsoloAll", "unsoloAll"}, {"/hush", "hush"}};
  static const char *const by_float[][2] = {{"/setcps", "streamSetCPS"}, {"/setbpm", "setbpm"}, {"/setCycle", "setCycle"}};
  char src[600];
  for (size_t i = 0; i < sizeof by_id / sizeof *by_id; i++)
    if (!strcmp(addr, by_id[i][0]) && nargs == 1 && (kind == 1 || kind == 2)) {
      size_t n = (size_t)snprintf(src, sizeof src, "%s (ID \"", by_id[i][1]);
      for (const char *c = arg; *c && n + 8 < sizeof src; c++) {
        if (*c == '"' || *c == '\\') src[n++] = '\\';
        src[n++] = *c;
      }
      snprintf(src + n, sizeof src - n, "\")");
      eval(src);
      return;
    }
  for (size_t i = 0; i < sizeof plain / sizeof *plain; i++)
    if (!strcmp(addr, plain[i][0]) && nargs == 0) {
      snprintf(src, sizeof src, "%s ()", plain[i][1]);
      eval(src);
      return;
    }
  for (size_t i = 0; i < sizeof by_float / sizeof *by_float; i++)
    if (!strcmp(addr, by_float[i][0]) && nargs == 1 && kind == 0) {
      snprintf(src, sizeof src, "%s (%s)", by_float[i][1], arg);
      eval(src);
      return;
    }
  if (!strcmp(addr, "/resetCycles")) {
    eval("resetCycles ()");
    return;
  }
  if (!strcmp(addr, "/nudgeAll") && nargs == 1 && kind == 3) {
    snprintf(src, sizeof src, "nudgeAll (%s)", arg);
    eval(src);
    return;
  }
  fprintf(stderr, "funk-midi: unhandled OSC: %s\n", addr);
}

/* A pattern given now plays from the next window: its first tick, as
   step's query rounds it up, so the pattern is not a tick early, in a
   window already sent. For trigger and first. */
double funk_stream_now(void) {
  int64_t u = (win - origin_us) * CYCLE_TICKS;
  return (double)(origin_tick + u / cs + (u % cs > 0)) / CYCLE_TICKS;
}

/* After pat may have changed. A pattern of Controls is d1's; for a
   Pattern Int, step is compiled again so it names the new pat. */
static void rebind(void) {
  char *type = funk_type_of(session, "pat", "<input>");
  bool of_controls = type && strstr(type, "Pattern") && strstr(type, "Controls");
  free(type);
  if (of_controls) {
    have_step = false;
    eval("d1 pat");
    return;
  }
  funk_control_float(&controls, "_t_pattern", funk_stream_now());
  if (dirt) fputs("funk-midi: pat is not playing: with --dirt it must be a pattern of Controls\n", stderr);
  else if (eval(STEP)) have_step = true;
  else fputs("funk-midi: pat is not playing: it must be a Pattern Int or of Controls\n", stderr);
}


int funk_stream_control(const char *name, void *stable) {
  funk_control *c = funk_control_at(&controls, name);
  if (!c) return 0;
  c->kind = 3, c->num.i = (int64_t)(intptr_t)stable;
  return 1;
}

void *funk_stream_controls(void) { return controls.e; }
long funk_stream_ncontrols(void) { return controls.n; }

/* After controls may have changed: its sources, from scratch. setF's
   values stay, as no source sets them. */
static void rebind_controls(void) {
  for (int i = 0; i < nosc; i++) funk_osc_close(osc_fds[i]);
  nosc = 0, cc_on = false;
  long kept = 0;
  for (long i = 0; i < controls.n; i++)
    if (controls.e[i].kind == 3) controls.e[kept++] = controls.e[i];
  controls.n = kept;
  funk_control_float(&controls, "_t_pattern", funk_stream_now());
  long ins[8], n = 0, arg = (long)ins;
  if (!eval("controlIns buf = p.writeControlIns (plusPtr nullPtr buf) controls") || !funk_call(session, "controlIns", 1, &arg, &n)) {
    fputs("funk-midi: controls is not used: it must be a [p.ControlIn]\n", stderr);
    return;
  }
  for (long i = 0; i < n && i < 8; i++) {
    if (ins[i] == -1) cc_on = true;
    else if (ins[i] == -2) fputs("funk-midi: p.params is the plugin's; funk-midi has none\n", stderr);
    else if ((osc_fds[nosc] = funk_osc_open((int)ins[i])) >= 0) nosc++;
    else fprintf(stderr, "funk-midi: cannot listen for OSC on port %ld\n", ins[i]);
  }
}

static bool defines(const char *s, const char *name) {
  s += strspn(s, " \t");
  size_t n = strlen(name);
  return strncmp(s, name, n) == 0 && (s[n] == ' ' || s[n] == '=' || s[n] == '\t');
}

/* a line of file path defines name at the top level */
static bool file_defines(const char *path, const char *name) {
  FILE *f = fopen(path, "r");
  char line[512];
  bool found = false;
  while (f && !found && fgets(line, sizeof line, f)) found = line[0] != ' ' && line[0] != '\t' && defines(line, name);
  if (f) fclose(f);
  return found;
}

/* After sig may have changed. */
static void rebind_sig(void) {
  if (!audio) return;
  if (!eval(playsig_src)) {
    fputs("funk-midi: sig is not playing: it must be a g.Sig\n", stderr);
    return;
  }
  have_sig = true;
  play_sig();
}

/* One line of input; false to quit. */
static bool handle(char *line) {
  line[strcspn(line, "\n")] = 0;
  char *s = line + strspn(line, " \t");
  if (!*s || !strncmp(s, "--", 2)) return true;
  if (*s != ':') {
    bool ok = eval(s);
    if (ok && defines(s, "pat")) rebind();
    if (ok && defines(s, "sig")) rebind_sig();
    if (ok && defines(s, "controls")) rebind_controls();
    return true;
  }
  /* :cc CHANNEL NUM VALUE, a CC as if it came in, channel 1 to 16 */
  /* :osc ADDR [ARG ...], a message as if it came in: an argument an int,
     a float (with a point), a double (a float ending in d) or a string */
  if (!strncmp(s, ":osc ", 5)) {
    unsigned char d[512];
    size_t n = 0;
    char *words[9], *save = NULL;
    int nw = 0;
    for (char *w = strtok_r(s + 5, " \t", &save); w && nw < 9; w = strtok_r(NULL, " \t", &save)) words[nw++] = w;
    if (!nw) return true;
#define OSC_STR(x) do { size_t l = strlen(x) + 1; if (n + l + 4 > sizeof d) return true; memcpy(d + n, x, l); n += l; while (n % 4) d[n++] = 0; } while (0)
    char tags[12] = ",";
    for (int i = 1; i < nw; i++) {
      size_t l = strlen(words[i]);
      tags[i] = strspn(words[i], "-0123456789") == l ? 'i' : strchr(words[i], '.') && strspn(words[i], "-0123456789.") == l ? 'f'
              : l > 1 && words[i][l - 1] == 'd' && strspn(words[i], "-0123456789.") == l - 1 ? 'd' : 's';
    }
    tags[nw] = 0;
    OSC_STR(words[0]);
    OSC_STR(tags);
    for (int i = 1; i < nw; i++) {
      if (tags[i] == 's') { OSC_STR(words[i]); continue; }
      if (tags[i] == 'd') {
        double x = atof(words[i]);
        uint64_t b;
        memcpy(&b, &x, 8);
        if (n + 8 > sizeof d) return true;
        for (int k = 7; k >= 0; k--) d[n++] = (unsigned char)(b >> (8 * k));
        continue;
      }
      uint32_t u;
      if (tags[i] == 'i') u = (uint32_t)(int32_t)atoi(words[i]);
      else { float f = (float)atof(words[i]); memcpy(&u, &f, 4); }
      if (n + 4 > sizeof d) return true;
      d[n++] = (unsigned char)(u >> 24), d[n++] = (unsigned char)(u >> 16), d[n++] = (unsigned char)(u >> 8), d[n++] = (unsigned char)u;
    }
#undef OSC_STR
    funk_osc_take_commands(d, n, &controls, osc_command, NULL);
    return true;
  }
  if (!strncmp(s, ":cc ", 4)) {
    int ch, num, v;
    if (sscanf(s + 4, "%d %d %d", &ch, &num, &v) == 3 && ch >= 1 && ch <= 16 && num >= 0 && num < 128 && v >= 0 && v < 128) cc_set(ch - 1, num, v);
    else fputs("funk-midi: :cc needs a channel 1 to 16, a number and a value 0 to 127\n", stderr);
    return true;
  }
  char *arg = s + strcspn(s, " \t"); /* after the command's name */
  arg += strspn(arg, " \t");
  switch (s[1]) {
  case 'q': return false;
  case 'l':
    if (funk_load_file(session, arg)) {
      rebind();
      rebind_sig();
      if (file_defines(arg, "controls")) rebind_controls();
    }
    return true;
  case 'w': advance_until(now_us() + (int64_t)(atof(arg) * 1000)); return true;
  case 's':
    playing = false;
    hush();
    if (audio) voice_hush();
    play_sig();
    return true;
  case 'p': playing = true; play_sig(); return true;
  case 'c':
    if (atof(arg) >= 1) set_cycle((int64_t)(atof(arg) * 1000));
    else fputs("funk-midi: :cycle needs a length in ms\n", stderr);
    return true;
  default:
    fputs("commands: :l FILE, :cycle MS, :wait MS, :cc CH NUM VALUE, :osc ADDR [ARG ...], :stop, :play, :q\n", stderr);
    return true;
  }
}

/* lines from stdin: the input thread hands one over and waits until it is handled */
static char *pending;
static bool input_eof;
static pthread_mutex_t lmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lcond = PTHREAD_COND_INITIALIZER;

static void *reader(void *arg) {
  (void)arg;
  for (;;) {
    fputs("funk-midi> ", stdout);
    fflush(stdout);
    char *line = NULL;
    size_t cap = 0;
    bool eof = getline(&line, &cap, stdin) < 0;
    pthread_mutex_lock(&lmu);
    if (eof) {
      free(line);
      input_eof = true;
      pthread_cond_signal(&lcond);
      pthread_mutex_unlock(&lmu);
      return NULL;
    }
    pending = line;
    pthread_cond_signal(&lcond);
    while (pending) pthread_cond_wait(&lcond, &lmu);
    pthread_mutex_unlock(&lmu);
  }
}

static void on_interrupt(int sig) {
  (void)sig;
  interrupted = 1;
}

/* --------------------------------------------------------------- main */

typedef struct { const char *lib, *file, *script, *kernels; } Opts;

/* graph.fk's imports: this program's functions, given to funk rather than
   exported, as a plugin must */
#define GRAPH_FUNCTIONS(X)                                                                                     \
  X(funk_graph_table_file) X(funk_graph_table_values) X(funk_table_error) X(funk_sample_path) X(funk_graph_new) X(funk_graph_add) X(funk_graph_control) X(funk_graph_name) X(funk_graph_output) X(funk_graph_memory)            \
  X(funk_player_new) X(funk_player_send) X(funk_player_frames) X(funk_player_play)                            \
  X(funk_player_render_channels) X(funk_player_free)
#define PROVIDE(f) funk_provide(session, #f, (void *)f);
/* stream.fk's imports (stream.h) */
#define STREAM_FUNCTIONS(X)                                                                                     \
  X(funk_stream_replace) X(funk_stream_get) X(funk_stream_count) X(funk_stream_key) X(funk_stream_at)          \
  X(funk_stream_first) X(funk_stream_firsts) X(funk_stream_first_at) X(funk_stream_expired)                    \
  X(funk_stream_set_global) X(funk_stream_global) X(funk_stream_now) X(funk_stream_random) X(funk_stream_cps)    \
  X(funk_stream_set_cps) X(funk_stream_cycle_now) X(funk_stream_set_cycle) X(funk_stream_nudge) X(funk_stream_set_nudge)        \
  X(funk_stream_set_state) X(funk_stream_control) X(funk_stream_controls) X(funk_stream_ncontrols)
#define PROVIDE_F(fn) funk_provide(f, #fn, (void *)fn);
extern const funk_glue_entry funk_glue_table[]; /* glue.c, from glue.fk by funk emit-c --glue */
extern const int funk_glue_count;

static void *handshake(void *arg) {
  (void)arg;
  dirt_handshake(dirt_fd, &dirt_stop, &dirt_connected, stderr);
  return NULL;
}

static int run(funk *f, void *arg) {
  Opts *o = arg;
  session = f;
  if (audio) { GRAPH_FUNCTIONS(PROVIDE) }
  funk_module_dir(f, o->lib);
  STREAM_FUNCTIONS(PROVIDE_F)
  funk_glue(f, funk_glue_table, funk_glue_count);
  if (!eval("import pattern as p") || !eval("import pattern (..)") || !eval("import midi as m") || !eval(CONTROL_IMPORT) || !eval(STREAM_IMPORT) ||
      !eval(dirt ? SLOTS_DIRT : SLOTS) || !eval(REVERT))
    return 1;
  if (audio && (!eval("import graph as g") || !eval("import dsp as d") || !start_audio(o->kernels))) return 1;
  t0 = mono_us();
  if (dirt) origin_us = win = lookahead_us; /* cycle 0 a lookahead after start-up, so its bundles arrive before their time */
  if (virtual_time) unix_at_t0 = -2208988800LL * 1000000; /* time tags then read as ms since start */
  else {
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    unix_at_t0 = (int64_t)rt.tv_sec * 1000000 + rt.tv_nsec / 1000 - (mono_us() - t0);
  }
  pthread_t send_th = 0;
  if (!virtual_time && pthread_create(&send_th, NULL, sender, NULL)) { perror("funk-midi: pthread_create"); return 1; }
  if (dirt_fd >= 0) {
    pthread_t hs_th;
    if (pthread_create(&hs_th, NULL, handshake, NULL)) { perror("funk-midi: pthread_create"); return 1; }
    pthread_detach(hs_th); /* it may sit in poll at exit */
  }
  if (o->file && funk_load_file(f, o->file)) {
    rebind();
    rebind_sig();
    if (file_defines(o->file, "controls")) rebind_controls();
  }

  if (o->script) {
    FILE *f = fopen(o->script, "r");
    if (!f) { perror(o->script); return 1; }
    char *line = NULL;
    size_t cap = 0;
    while (!interrupted && getline(&line, &cap, f) >= 0 && handle(line)) {}
    free(line);
    fclose(f);
  } else {
    pthread_t read_th;
    if (pthread_create(&read_th, NULL, reader, NULL)) { perror("funk-midi: pthread_create"); return 1; }
    pthread_detach(read_th); /* it may sit in getline at exit */
    for (;;) {
      schedule();
      pthread_mutex_lock(&lmu);
      struct timespec ts = after_us(win - lookahead_us - now_us());
      while (!pending && !input_eof && !interrupted && pthread_cond_timedwait(&lcond, &lmu, &ts) == 0) {}
      char *line = pending;
      bool eof = input_eof;
      pthread_mutex_unlock(&lmu);
      if (interrupted || (!line && eof)) break;
      if (!line) continue;
      bool more = handle(line);
      free(line);
      pthread_mutex_lock(&lmu);
      pending = NULL;
      pthread_cond_signal(&lcond);
      pthread_mutex_unlock(&lmu);
      if (!more) break;
    }
  }

  hush();
  if (audio) stop_audio();
  if (virtual_time) {
    flush_until(INT64_MAX);
  } else {
    pthread_mutex_lock(&mu);
    quitting = true;
    pthread_cond_signal(&queued);
    pthread_mutex_unlock(&mu);
    pthread_join(send_th, NULL);
  }
  return 0;
}

static void usage(FILE *f) {
  fputs("usage: funk-midi [FILE] [--script FILE] [--port N | --virtual NAME] [--list]\n"
        "                 [--cycle MS] [--print] [--no-sleep] [-M DIR] [--audio [--kernels LIB] [--voices N]]\n"
        "                 [--dirt [HOST:]PORT]\n"
        "Plays Tidal's slots, `d1 $ s \"bd\"` and `p \"name\" $ ...`, and `pat`, a Pattern\n"
        "Int or of Controls (d1's), to a MIDI output, or to SuperDirt with --dirt;\n"
        "FILE is loaded first.\n"
        "  --script FILE  run FILE's lines as input, then exit; `:wait MS` plays for MS\n"
        "  --port N       output port N; --list shows them. Default: a virtual port\n"
        "  --virtual NAME a virtual output port named NAME (default funk-midi)\n"
        "  --in N         MIDI input port N, for controls' p.midiCC; --list shows them\n"
        "  --in-virtual NAME  a virtual MIDI input named NAME\n"
        "  --cycle MS     the length of a cycle (default 1739.13: Tidal's 0.575 cycles a second)\n"
        "  --print        print the messages instead of sending them\n"
        "  --no-sleep     run on a virtual clock, as fast as possible; implies --print\n"
        "  -M DIR         search DIR for modules (default " FUNK_LIB_DIR ")\n"
        "  --audio        also play `sig`, a g.Sig of dsp.fk's signals, to the audio output\n"
        "  --kernels LIB  the kernels' library (default " FUNK_DSP_LIB ")\n"
        "  --voices N     voices for pat's notes: `voices f` sums N of f gate freq (default 8)\n"
        "  --dirt [HOST:]PORT  send the slots to SuperDirt as Tidal does, instead of\n"
        "                 MIDI (SuperDirt's: 57120)\n"
        "Input is funk REPL input, with `pattern as p` and `midi as m` imported (and\n"
        "`graph as g` and `dsp as d` with --audio), or\n"
        ":l FILE, :cycle MS, :wait MS, :cc CH NUM VALUE, :osc ADDR [ARG ...], :stop, :play, :q\n"
        "A top-level `controls`, such as [p.osc 6010, p.midiCC], gives pat Tidal's cF\n"
        "and the rest: OSC /ctrl on a port, and CC from --in or :cc.\n",
        f);
}

int main(int argc, char **argv) {
  Opts o = {FUNK_LIB_DIR, NULL, NULL, FUNK_DSP_LIB};
  int port = -1, in_port = -2; /* -2: no input */
  const char *vname = "funk-midi", *in_name = NULL, *dirt_to = NULL;
  bool list = false;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
    else if (!strcmp(a, "--list")) list = true;
    else if (!strcmp(a, "--print")) print_sink = true;
    else if (!strcmp(a, "--no-sleep")) virtual_time = print_sink = true;
    else if (next && !strcmp(a, "--script")) o.script = argv[++i];
    else if (next && !strcmp(a, "--port")) port = atoi(argv[++i]);
    else if (next && !strcmp(a, "--virtual")) vname = argv[++i];
    else if (next && !strcmp(a, "--in")) in_port = atoi(argv[++i]);
    else if (next && !strcmp(a, "--in-virtual")) in_port = -1, in_name = argv[++i];
    else if (next && !strcmp(a, "--cycle")) cs = (int64_t)(atof(argv[++i]) * 1000), cps = cs ? 1e6 / (double)cs : 0;
    else if (next && !strcmp(a, "-M")) o.lib = argv[++i];
    else if (!strcmp(a, "--audio")) audio = true;
    else if (next && !strcmp(a, "--dirt")) dirt = true, dirt_to = argv[++i];
    else if (next && !strcmp(a, "--kernels")) o.kernels = argv[++i];
    else if (next && !strcmp(a, "--voices")) nvoices = atoi(argv[++i]);
    else if (a[0] != '-' && !o.file) o.file = a;
    else { usage(stderr); return 2; }
  }
  if (!cs) cs = llround(1e6 / cps);
  if (cs < 1000) { fputs("funk-midi: --cycle needs at least 1 ms\n", stderr); return 2; }
  if (dirt) {
    char host[256] = "127.0.0.1";
    const char *colon = strrchr(dirt_to, ':');
    if (colon) snprintf(host, sizeof host, "%.*s", (int)(colon - dirt_to), dirt_to);
    int dport = atoi(colon ? colon + 1 : dirt_to);
    if (dport < 1 || dport > 65535) { fputs("funk-midi: --dirt needs a port, as 57120 or 127.0.0.1:57120\n", stderr); return 2; }
    if (!print_sink && (dirt_fd = dirt_open(host, dport)) < 0) { fprintf(stderr, "funk-midi: cannot reach %s:%d\n", host, dport); return 1; }
    lookahead_us = 100000;
  }
  if (nvoices < 1 || nvoices > MAX_VOICES) { fprintf(stderr, "funk-midi: --voices needs 1 to %d\n", MAX_VOICES); return 2; }
#ifdef HAVE_LIBREMIDI
  if (list) { list_ports(); return 0; }
  if (!print_sink && !dirt && !open_out(port, vname)) return 1;
  if (in_port != -2 && !open_in(in_port, in_name)) return 1;
#else
  (void)port, (void)vname, (void)in_name;
  if (list || (!print_sink && !dirt) || in_port != -2) {
    fputs("funk-midi: built without libremidi: only --print and --no-sleep work\n", stderr);
    return 1;
  }
#endif
#ifndef HAVE_MINIAUDIO
  if (audio && !virtual_time) {
    fputs("funk-midi: built without miniaudio: --audio works only with --no-sleep\n", stderr);
    return 1;
  }
#endif
  signal(SIGINT, on_interrupt);
  static const char *const cc_args[] = {"-I", FUNK_GRAPH_INCLUDE}; /* graph.h, for graph.fk's imports */
  funk *f = funk_new(&(funk_config){.cc_args = audio ? cc_args : NULL, .ncc_args = audio ? 2 : 0});
  if (!f) return 1;
  int status = funk_run(f, run, &o);
  funk_free(f);
#ifdef HAVE_LIBREMIDI
  if (out) libremidi_midi_out_free(out);
  if (in) libremidi_midi_in_free(in);
#endif
  return status;
}
