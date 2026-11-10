/* stream.h -- a host's pattern slots, for lib/stream.fk (docs/dev/stream.md):
   Tidal's PlayMap, of StablePtrs to each slot's PlayState, plus the
   patterns `first` plays once and `streamAll`'s function. Only funk's
   thread calls these. */
#ifndef FUNK_STREAM_H
#define FUNK_STREAM_H

/* Slot id's state is stable, a StablePtr. Returns the StablePtr it had,
   for funk to free, or NULL. */
void *funk_stream_replace(const char *id, void *stable);

/* Slot id's StablePtr, or NULL. */
void *funk_stream_get(const char *id);

/* The slots, in Tidal's order (Map: by id, as strcmp): how many, and the
   ith's id and StablePtr. */
long funk_stream_count(void);
const char *funk_stream_key(long i);
void *funk_stream_at(long i);

/* A pattern played once, a StablePtr, until cycle `until`. */
void funk_stream_first(void *stable, double until);

/* The ith pattern played once; and one whose time is over, removed, for
   funk to free, or NULL. */
long funk_stream_firsts(void);
void *funk_stream_first_at(long i);
void *funk_stream_expired(void);

/* streamAll's function: sets it, returning the old one or NULL; or reads
   it. */
void *funk_stream_set_global(void *stable);
void *funk_stream_global(void);

/* The cycle a pattern given now starts at: the host's. */
double funk_stream_now(void);

/* The host's clock: cycles per second, set from the next window, clamped
   to Link's 20 to 999 bpm at 4 beats a cycle; the cycle now; setCycle,
   from the next window; nudgeAll's seconds. */
double funk_stream_cps(void);
void funk_stream_set_cps(double cps);
double funk_stream_cycle_now(void);
void funk_stream_set_cycle(double cycle);
double funk_stream_nudge(void);
void funk_stream_set_nudge(double seconds);

/* 0 to 8192, for once's cycle (Stream/UI.hs, streamOnce). */
long funk_stream_random(void);

#endif
