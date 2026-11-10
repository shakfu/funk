/* stream.c -- the slot table behind lib/stream.fk (stream.h). Only funk's
   thread uses it: replaces run in input, queries in step. */
#define _DEFAULT_SOURCE /* random, srandom */
#include "stream.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { char *id; void *stable; } Slot;
static Slot *slots;
static long nslots, capslots;

typedef struct { void *stable; double until; } First;
static First *firsts;
static long nfirsts, capfirsts;
static void *global;

static void *grow(void *a, long *cap, size_t size) {
  *cap = *cap ? 2 * *cap : 16;
  a = realloc(a, (size_t)*cap * size);
  if (!a) abort();
  return a;
}

void *funk_stream_replace(const char *id, void *stable) {
  long i = 0;
  while (i < nslots && strcmp(slots[i].id, id) < 0) i++;
  void *old = NULL;
  if (i < nslots && !strcmp(slots[i].id, id)) {
    old = slots[i].stable;
    slots[i].stable = stable;
    return old;
  }
  if (nslots == capslots) slots = grow(slots, &capslots, sizeof *slots);
  memmove(slots + i + 1, slots + i, (size_t)(nslots - i) * sizeof *slots);
  slots[i] = (Slot){strdup(id), stable};
  nslots++;
  return NULL;
}

void *funk_stream_get(const char *id) {
  for (long i = 0; i < nslots; i++)
    if (!strcmp(slots[i].id, id)) return slots[i].stable;
  return NULL;
}

long funk_stream_count(void) { return nslots; }
const char *funk_stream_key(long i) { return i >= 0 && i < nslots ? slots[i].id : ""; }
void *funk_stream_at(long i) { return i >= 0 && i < nslots ? slots[i].stable : NULL; }

void funk_stream_first(void *stable, double until) {
  if (nfirsts == capfirsts) firsts = grow(firsts, &capfirsts, sizeof *firsts);
  firsts[nfirsts++] = (First){stable, until};
}

long funk_stream_firsts(void) { return nfirsts; }
void *funk_stream_first_at(long i) { return i >= 0 && i < nfirsts ? firsts[i].stable : NULL; }

void *funk_stream_expired(void) {
  double now = funk_stream_now();
  for (long i = 0; i < nfirsts; i++)
    if (firsts[i].until <= now) {
      void *s = firsts[i].stable;
      memmove(firsts + i, firsts + i + 1, (size_t)(nfirsts - i - 1) * sizeof *firsts);
      nfirsts--;
      return s;
    }
  return NULL;
}

void *funk_stream_set_global(void *stable) {
  void *old = global;
  global = stable;
  return old;
}

void *funk_stream_global(void) { return global; }

static double nudge;
double funk_stream_nudge(void) { return nudge; }
void funk_stream_set_nudge(double seconds) { nudge = seconds; }

long funk_stream_random(void) {
  static int seeded;
  if (!seeded) srandom((unsigned)time(NULL)), seeded = 1;
  return random() % 8193;
}
