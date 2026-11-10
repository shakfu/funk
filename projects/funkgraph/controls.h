/* controls.h -- a host's table of controls, as lib/pattern.fk's readControls
   reads it (docs/dev/controls-in.md): 128-byte entries, each set by name.
   One thread writes and calls step; a source on another thread must hand
   its values to that one. */
#ifndef FUNK_CONTROLS_H
#define FUNK_CONTROLS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char name[48];
  int64_t kind; /* 0 f, 1 i, 2 text, 3 a StablePtr: funk-midi's setF */
  union {
    double f;
    int64_t i;
  } num;
  char text[64];
} funk_control;

enum { FUNK_CONTROLS = 256 };

typedef struct {
  long n;
  funk_control e[FUNK_CONTROLS];
} funk_controls;

/* name's entry, added if new; NULL if the name is too long or the table full */
static inline funk_control *funk_control_at(funk_controls *t, const char *name) {
  if (strlen(name) >= sizeof t->e[0].name) return NULL;
  for (long i = 0; i < t->n; i++)
    if (!strcmp(t->e[i].name, name)) return &t->e[i];
  if (t->n == FUNK_CONTROLS) return NULL;
  funk_control *c = &t->e[t->n++];
  memset(c, 0, sizeof *c);
  snprintf(c->name, sizeof c->name, "%s", name);
  return c;
}

static inline void funk_control_float(funk_controls *t, const char *name, double x) {
  funk_control *c = funk_control_at(t, name);
  if (c) c->kind = 0, c->num.f = x;
}

static inline void funk_control_int(funk_controls *t, const char *name, int64_t x) {
  funk_control *c = funk_control_at(t, name);
  if (c) c->kind = 1, c->num.i = x;
}

static inline void funk_control_text(funk_controls *t, const char *name, const char *x) {
  funk_control *c = funk_control_at(t, name);
  if (c) c->kind = 2, snprintf(c->text, sizeof c->text, "%s", x);
}

/* A CC's value, 0 to 127 on channel 0 to 15, as "cc<num>", any channel's
   last, and "<channel + 1>:cc<num>", from 0 to 1. */
static inline void funk_control_cc(funk_controls *t, int channel, int num, int value) {
  char name[16];
  snprintf(name, sizeof name, "cc%d", num);
  funk_control_float(t, name, value / 127.0);
  snprintf(name, sizeof name, "%d:cc%d", channel + 1, num);
  funk_control_float(t, name, value / 127.0);
}

#ifdef __cplusplus
extern "C" {
#endif

/* A UDP socket on 127.0.0.1:port for Tidal's OSC /ctrl, as Tidal's listener
   takes them; -1 if the port cannot be bound. It never blocks. */
int funk_osc_open(int port);

/* Every datagram waiting on fd, into t: /ctrl name value, a name a string
   or an int, which stands for its decimal string, and a value a float, an
   int or a string, alone or in bundles. Others are ignored. How many
   controls were set. */
int funk_osc_poll(int fd, funk_controls *t);

/* The /ctrl messages of one datagram of n bytes, into t; how many. */
int funk_osc_take(const unsigned char *data, size_t n, funk_controls *t);

/* funk_osc_poll, and every other message to cmd: its address, its number
   of arguments, and the first one's kind (0 float, 1 int, 2 string, 3
   double, -1 none or another) and text, as Tidal's listener takes its
   commands (Stream/Listen.hs). */
typedef void (*funk_osc_command)(void *ctx, const char *addr, int nargs, int kind, const char *arg);
int funk_osc_poll_commands(int fd, funk_controls *t, funk_osc_command cmd, void *ctx);
int funk_osc_take_commands(const unsigned char *data, size_t n, funk_controls *t, funk_osc_command cmd, void *ctx);

void funk_osc_close(int fd);

#ifdef __cplusplus
}
#endif

#endif
