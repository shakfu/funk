/* dirt.h -- SuperDirt's /dirt/play, as Tidal's superdirtTarget sends it,
   from the events lib/control.fk's writeDirtWith writes (docs/dev/dirt.md). */
#ifndef FUNK_DIRT_H
#define FUNK_DIRT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  int64_t start, dur; /* in the step's units */
  double cycle;
  int64_t n;          /* controls */
  const unsigned char *pairs, *next;
} dirt_event;

/* The event at *p, before end; false at the end mark or a malformed one. */
int dirt_next(const unsigned char *p, const unsigned char *end, dirt_event *e);

/* A control of e as a float, or dflt: for nudge. */
double dirt_float(const dirt_event *e, const char *name, double dflt);

/* An OSC bundle at NTP time tag holding e's /dirt/play: its controls,
   _id_ among them, and Tidal's cps, cycle and delta (seconds), unless e
   has its own, in order of name. Its size, or 0 if it does not fit in
   cap. */
size_t dirt_bundle(const dirt_event *e, uint64_t tag, double cps, double delta, unsigned char *out, size_t cap);

/* NTP time of us microseconds since the Unix epoch. */
uint64_t dirt_ntp(int64_t us);

/* Prints a bundle's time (ms since the NTP epoch, less base_ms) and its
   message's arguments, a float as its bits: for tests. */
void dirt_print(FILE *f, const unsigned char *b, size_t n, double base_ms);

/* A UDP socket sending to host:port; -1 if it cannot be made. */
int dirt_open(const char *host, int port);

/* Tidal's handshake on fd, until *stop: /dirt/handshake every 2 s until
   SuperDirt replies, again on /dirt/hello; "Waiting for SuperDirt" once,
   then "Connected to SuperDirt" once, to log. *connected is set on a reply. */
void dirt_handshake(int fd, volatile int *stop, volatile int *connected, FILE *log);

#endif
