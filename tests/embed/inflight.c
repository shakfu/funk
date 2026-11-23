/* funk_free waits for a queued callback between claim and publish (rt.c,
   fk_stop). A host thread claims a slot in the session's ring, then writes
   it 200 ms later, after funk_free has started. If funk_free freed the ring
   without waiting, it returned before the write, and the write used freed
   memory (AddressSanitizer reports it). */
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libfunk.h"
#include "rt.h" /* fk_event_claim and fk_event_publish, as a queued callback's trampoline calls them */

static void *stable;
static atomic_int claimed, written;

static int get_stable(funk *f, void *arg) {
  (void)arg;
  char *shown;
  if (!funk_eval(f, "primPtrToInt (castStablePtrToPtr (newStablePtr 0))", "<inflight>", &shown)) return 1;
  stable = (void *)(uintptr_t)strtoull(shown, NULL, 10);
  free(shown);
  return 0;
}

static void *sender(void *arg) {
  (void)arg;
  FkEvent *e = fk_event_claim(stable);
  atomic_store(&claimed, e ? 1 : -1);
  if (!e) return NULL;
  usleep(200 * 1000);
  e->run = NULL; /* dropped: nothing reads it */
  memset(e->data, 0, sizeof e->data);
  atomic_store(&written, 1);
  fk_event_publish(e);
  return NULL;
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void) {
  funk *f = funk_new(NULL);
  if (!f || funk_run(f, get_stable, NULL)) return 1;
  pthread_t th;
  pthread_create(&th, NULL, sender, NULL);
  while (!atomic_load(&claimed)) sched_yield();
  if (atomic_load(&claimed) < 0) return 1;
  double t0 = now();
  funk_free(f);
  double waited = now() - t0;
  printf("written before funk_free returned: %s\n", atomic_load(&written) ? "yes" : "no");
  printf("funk_free waited: %s\n", waited > 0.1 ? "yes" : "no");
  pthread_join(th, NULL);
  printf("a claim after funk_free: %s\n", fk_event_claim(stable) ? "slot" : "none");
  return 0;
}
