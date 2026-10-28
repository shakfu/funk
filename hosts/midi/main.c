/* funk-midi -- plays a funk pattern to a MIDI port, live (docs/dev/patterns.md).

   A REPL over libfunk.h, with `pattern as p` and `midi as m` imported.
   Whenever input defines `pat`, a Pattern Int (lib/midi.fk gives the
   values), it plays from the next window. A clock runs from start-up, so
   patterns stay on one cycle grid across redefinitions.

   Threads: funk's runs the REPL input and, every 10 ms window, calls
   `step` through funk_call for the events 50 ms ahead; a sender thread
   sends them at their times, so funk's work never delays a message; an
   input thread reads lines. --no-sleep runs on a virtual clock instead,
   printing each message: for tests. */
#define _DEFAULT_SOURCE
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libfunk.h"

#ifdef HAVE_LIBREMIDI
#include <libremidi/libremidi-c.h>
#endif

#ifndef FUNK_LIB_DIR
#define FUNK_LIB_DIR "lib"
#endif

enum { WINDOW_US = 10000, LOOKAHEAD_US = 50000, CAP = 512, CYCLE_TICKS = 720720 };
#define STEP "step buf cs ot a b = p.writeEvents (plusPtr nullPtr buf) 512 (p.shift (negate ot) pat) cs a b"

static bool virtual_time, print_sink;
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

static void list_ports(void) {
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
static void send_msg(int64_t t, const unsigned char *m) {
  if (print_sink) {
    if (!virtual_time) t = now_us();
    printf("%9.3f %s ch%d %d %d\n", (double)t / 1000, (m[0] & 0xf0) == 0x90 ? "on " : "off", (m[0] & 15) + 1, m[1], m[2]);
    fflush(stdout);
    return;
  }
#ifdef HAVE_LIBREMIDI
  libremidi_midi_out_send_message(out, m, 3);
#endif
}

/* ------------------------------------------------- the queue of messages */

typedef struct { int64_t t; uint64_t seq; unsigned char m[3]; } Msg;
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

static void enqueue(int64_t t, unsigned char status, unsigned char d1, unsigned char d2) {
  pthread_mutex_lock(&mu);
  heap_push((Msg){t, nseq++, {status, d1, d2}});
  pthread_cond_signal(&queued);
  pthread_mutex_unlock(&mu);
}

/* Drops the queued note-ons and moves the note-offs to now: every note
   sounding or due stops. */
static void hush(void) {
  pthread_mutex_lock(&mu);
  size_t n = nheap;
  Msg *all = malloc(n * sizeof *all);
  if (!all) { fputs("funk-midi: out of memory\n", stderr); exit(1); }
  memcpy(all, heap, n * sizeof *all);
  nheap = 0;
  int64_t now = now_us();
  for (size_t i = 0; i < n; i++)
    if ((all[i].m[0] & 0xf0) == 0x80) heap_push((Msg){now, all[i].seq, {all[i].m[0], all[i].m[1], all[i].m[2]}});
  free(all);
  pthread_cond_signal(&queued);
  pthread_mutex_unlock(&mu);
}

/* virtual time: sends what is due before t */
static void flush_until(int64_t t) {
  while (nheap && heap[0].t < t) {
    Msg m = heap_pop();
    send_msg(m.t, m.m);
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
    send_msg(m.t, m.m);
    pthread_mutex_lock(&mu);
  }
  pthread_mutex_unlock(&mu);
  return NULL;
}

/* --------------------------------------------------------- the pattern */

static long buf[3 * CAP]; /* start, duration, value */
static bool playing = true, have_step;
static int64_t cs = 2000000; /* microseconds per cycle */
static int64_t origin_us, origin_tick, win; /* tick origin_tick sounds at origin_us; win: the next window */

/* Calls `step` for every window that starts within the lookahead. */
static void schedule(void) {
  while (win <= now_us() + LOOKAHEAD_US) {
    if (playing && have_step) {
      long args[5] = {(long)buf, (long)cs, (long)origin_tick, (long)(win - origin_us), (long)(win + WINDOW_US - origin_us)}, n;
      if (!funk_call("step", 5, args, &n)) {
        have_step = false; /* a runtime error, reported: silence until pat is defined again */
        fputs("funk-midi: pat failed; stopped until it is defined again\n", stderr);
      } else {
        if (n > CAP) fprintf(stderr, "funk-midi: %ld events in a window; played %d\n", n, CAP);
        for (long i = 0; i < n && i < CAP; i++) {
          int64_t at = origin_us + buf[3 * i], dur = buf[3 * i + 1] > 0 ? buf[3 * i + 1] : 1;
          long v = buf[3 * i + 2];
          unsigned char note = v & 127, vel = (v >> 7) & 127, ch = (v >> 14) & 15;
          enqueue(at, (unsigned char)(0x90 | ch), note, vel ? vel : 100);
          enqueue(at + dur, (unsigned char)(0x80 | ch), note, 0);
        }
      }
    }
    win += WINDOW_US;
  }
}

/* Plays until time t. */
static void advance_until(int64_t t) {
  while (now_us() < t && !interrupted) {
    schedule();
    int64_t next = win - LOOKAHEAD_US; /* above now: schedule ran */
    if (next > t) next = t;
    if (virtual_time) {
      flush_until(next);
      vnow = next;
    } else {
      int64_t d = next - now_us();
      if (d > 0) usleep((useconds_t)d);
    }
  }
}

static void set_cycle(int64_t us) { /* from the next unscheduled window, so the beat stays */
  origin_tick += (win - origin_us) * CYCLE_TICKS / cs;
  origin_us = win;
  cs = us;
}

/* -------------------------------------------------------------- input */

static bool eval(const char *src) {
  char *shown;
  bool ok = funk_eval(src, "<input>", &shown);
  if (shown) puts(shown);
  free(shown);
  return ok;
}

/* After pat may have changed: step is compiled again so it names the new pat. */
static void rebind(void) {
  if (eval(STEP)) have_step = true;
  else fputs("funk-midi: pat is not playing: it must be a Pattern Int\n", stderr);
}

static bool defines_pat(const char *s) {
  s += strspn(s, " \t");
  return strncmp(s, "pat", 3) == 0 && (s[3] == ' ' || s[3] == '=' || s[3] == '\t');
}

/* One line of input; false to quit. */
static bool handle(char *line) {
  line[strcspn(line, "\n")] = 0;
  char *s = line + strspn(line, " \t");
  if (!*s || !strncmp(s, "--", 2)) return true;
  if (*s != ':') {
    if (eval(s) && defines_pat(s)) rebind();
    return true;
  }
  char *arg = s + strcspn(s, " \t"); /* after the command's name */
  arg += strspn(arg, " \t");
  switch (s[1]) {
  case 'q': return false;
  case 'l':
    if (funk_load_file(arg)) rebind();
    return true;
  case 'w': advance_until(now_us() + (int64_t)(atof(arg) * 1000)); return true;
  case 's': playing = false; hush(); return true;
  case 'p': playing = true; return true;
  case 'c':
    if (atof(arg) >= 1) set_cycle((int64_t)(atof(arg) * 1000));
    else fputs("funk-midi: :cycle needs a length in ms\n", stderr);
    return true;
  default:
    fputs("commands: :l FILE, :cycle MS, :wait MS, :stop, :play, :q\n", stderr);
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

typedef struct { const char *lib, *file, *script; } Opts;

static int run(void *arg) {
  Opts *o = arg;
  if (!funk_init(NULL, 0)) return 1;
  funk_module_dir(o->lib);
  if (!eval("import pattern as p") || !eval("import midi as m")) return 1;
  t0 = mono_us();
  pthread_t send_th = 0;
  if (!virtual_time && pthread_create(&send_th, NULL, sender, NULL)) { perror("funk-midi: pthread_create"); return 1; }
  if (o->file && funk_load_file(o->file)) rebind();

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
      struct timespec ts = after_us(win - LOOKAHEAD_US - now_us());
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
        "                 [--cycle MS] [--print] [--no-sleep] [-M DIR]\n"
        "Plays `pat`, a Pattern Int, to a MIDI output; FILE is loaded first.\n"
        "  --script FILE  run FILE's lines as input, then exit; `:wait MS` plays for MS\n"
        "  --port N       output port N; --list shows them. Default: a virtual port\n"
        "  --virtual NAME a virtual output port named NAME (default funk-midi)\n"
        "  --cycle MS     the length of a cycle (default 2000)\n"
        "  --print        print the messages instead of sending them\n"
        "  --no-sleep     run on a virtual clock, as fast as possible; implies --print\n"
        "  -M DIR         search DIR for modules (default " FUNK_LIB_DIR ")\n"
        "Input is funk REPL input, with `pattern as p` and `midi as m` imported, or\n"
        ":l FILE, :cycle MS, :wait MS, :stop, :play, :q\n",
        f);
}

int main(int argc, char **argv) {
  Opts o = {FUNK_LIB_DIR, NULL, NULL};
  int port = -1;
  const char *vname = "funk-midi";
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
    else if (next && !strcmp(a, "--cycle")) cs = (int64_t)(atof(argv[++i]) * 1000);
    else if (next && !strcmp(a, "-M")) o.lib = argv[++i];
    else if (a[0] != '-' && !o.file) o.file = a;
    else { usage(stderr); return 2; }
  }
  if (cs < 1000) { fputs("funk-midi: --cycle needs at least 1 ms\n", stderr); return 2; }
#ifdef HAVE_LIBREMIDI
  if (list) { list_ports(); return 0; }
  if (!print_sink && !open_out(port, vname)) return 1;
#else
  (void)port, (void)vname;
  if (list || !print_sink) {
    fputs("funk-midi: built without libremidi: only --print and --no-sleep work\n", stderr);
    return 1;
  }
#endif
  signal(SIGINT, on_interrupt);
  int status = funk_main(run, &o);
#ifdef HAVE_LIBREMIDI
  if (out) libremidi_midi_out_free(out);
#endif
  return status;
}
