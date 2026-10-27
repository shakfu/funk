/* A library outside the default search paths, for tests/run.sh: funk run
   and funk build find it with -I, -L and -l. Its callbacks take a context
   first, as libremidi's do. */
#ifndef FKT_H
#define FKT_H
#include <stddef.h>
#include <stdint.h>

int fkt_scale(int x, float f);
size_t fkt_twice(size_t n);
unsigned fkt_wrap(unsigned x);
#define FKT_LIMIT 255u

/* Callbacks on the caller's thread, during the call. */
void fkt_each(int n, void *ctx, void (*cb)(void *ctx, int i));
int fkt_fold(int n, void *ctx, int (*cb)(void *ctx, int acc, int x));

/* A message source on its own thread, configured through a struct whose
   callback sits in an anonymous union, like libremidi_midi_configuration. */
typedef struct fkt_config {
  int version;
  union {
    struct {
      void *context;
      void (*callback)(void *ctx, int64_t ts, const unsigned char *bytes, size_t len);
    } on_message;
  };
  int count;
  int length; /* bytes per message: byte j is (i + j) % 256; 0 means a 3-byte note-on */
} fkt_config;
#define FKT_V1 2
int fkt_start(const fkt_config *cfg); /* 0 on success */
void fkt_join(void);

/* An audio host: a thread that calls cb(ctx, buf, frames) blocks times
   with a buffer of frames floats, and sums every sample written. */
int fkt_audio_start(void (*cb)(void *ctx, float *buf, int frames), void *ctx, int frames, int blocks);
double fkt_audio_join(void);

/* Reads n bytes of /dev/zero into a buffer on its own stack: the kernel
   writes there, as read() and stat() do. The bytes read, or -1. */
long fkt_read_stack(long n);

/* Calls cb from another thread with NULL, then with "after", and waits. */
void fkt_send_null(void *ctx, void (*cb)(void *ctx, const char *s));

/* Calls cb from another thread: wrong for a direct callback. */
void fkt_other_thread(void *ctx, void (*cb)(void *ctx, int i));

#endif
