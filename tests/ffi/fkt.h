/* A library outside the default search paths, for tests/run.sh: funk run
   and funk build find it with -I, -L and -l. Its callbacks take a context
   first, as libremidi's do. */
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
} fkt_config;
#define FKT_V1 2
int fkt_start(const fkt_config *cfg); /* 0 on success */
void fkt_join(void);

/* Calls cb from another thread: wrong for a direct callback. */
void fkt_other_thread(void *ctx, void (*cb)(void *ctx, int i));
