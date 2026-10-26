#include "fkt.h"

#include <pthread.h>

int fkt_scale(int x, float f) { return (int)((float)x * f); }
size_t fkt_twice(size_t n) { return 2 * n; }
unsigned fkt_wrap(unsigned x) { return x + 1; }

void fkt_each(int n, void *ctx, void (*cb)(void *ctx, int i)) {
  for (int i = 0; i < n; i++) cb(ctx, i);
}

int fkt_fold(int n, void *ctx, int (*cb)(void *ctx, int acc, int x)) {
  int acc = 0;
  for (int i = 1; i <= n; i++) acc = cb(ctx, acc, i);
  return acc;
}

static fkt_config cfg;
static pthread_t source;

/* Note-on messages 0x90 i 100, with time stamps 10 i. */
static void *send(void *arg) {
  (void)arg;
  for (int i = 0; i < cfg.count; i++) {
    unsigned char msg[3] = {0x90, (unsigned char)i, 100};
    cfg.on_message.callback(cfg.on_message.context, 10 * i, msg, sizeof msg);
  }
  return NULL;
}

int fkt_start(const fkt_config *c) {
  if (c->version != FKT_V1 || !c->on_message.callback) return -1;
  cfg = *c;
  return pthread_create(&source, NULL, send, NULL);
}

void fkt_join(void) { pthread_join(source, NULL); }

typedef struct { void *ctx; void (*cb)(void *, int); } other;

static void *call_other(void *arg) {
  other *o = arg;
  o->cb(o->ctx, 1);
  return NULL;
}

void fkt_other_thread(void *ctx, void (*cb)(void *ctx, int i)) {
  pthread_t t;
  other o = {ctx, cb};
  pthread_create(&t, NULL, call_other, &o);
  pthread_join(t, NULL);
}
