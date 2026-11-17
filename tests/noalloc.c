/* noalloc.c -- counts allocation, freeing and locking on one armed thread.
   Loaded with DYLD_INSERT_LIBRARIES (macOS) or LD_PRELOAD (Linux), so it
   also sees calls from inside libc and libc++. A test finds noalloc_arm
   and noalloc_hits with dlsym; the first hit prints a backtrace. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the armed thread; no thread-local storage, whose first use may allocate */
static _Atomic(pthread_t) armed;
static atomic_int on, hits;

void noalloc_arm(int yes) {
  if (yes) atomic_store(&armed, pthread_self());
  atomic_store(&on, yes);
}

int noalloc_hits(void) { return atomic_load(&hits); }

static void hit(const char *what) {
  if (!atomic_load_explicit(&on, memory_order_relaxed) || !pthread_equal(atomic_load(&armed), pthread_self())) return;
  if (atomic_fetch_add(&hits, 1)) return;
  atomic_store(&on, 0); /* backtrace may allocate */
  void *frames[32];
  int n = backtrace(frames, 32);
  static const char head[] = "noalloc: ";
  write(2, head, sizeof head - 1);
  write(2, what, strlen(what));
  write(2, " on the armed thread\n", 21);
  backtrace_symbols_fd(frames, n, 2);
  atomic_store(&on, 1);
}

#ifdef __APPLE__
/* children, such as a kernel's cc, are not under test; with SIP off, the insert kills an arm64e cc.
   Linux keeps LD_PRELOAD: check.sh inserts it through setarch, which must pass it on. */
__attribute__((constructor)) static void uninherit(void) { unsetenv("DYLD_INSERT_LIBRARIES"); }

static void *n_malloc(size_t n) { hit("malloc"); return malloc(n); }
static void *n_calloc(size_t a, size_t b) { hit("calloc"); return calloc(a, b); }
static void *n_realloc(void *p, size_t n) { hit("realloc"); return realloc(p, n); }
static void n_free(void *p) { if (p) hit("free"); free(p); }
static int n_posix_memalign(void **p, size_t a, size_t n) { hit("posix_memalign"); return posix_memalign(p, a, n); }
static void *n_aligned_alloc(size_t a, size_t n) { hit("aligned_alloc"); return aligned_alloc(a, n); }
static int n_lock(pthread_mutex_t *m) { hit("pthread_mutex_lock"); return pthread_mutex_lock(m); }

#define INTERPOSE(mine, theirs) \
  __attribute__((used)) static const struct { const void *r, *o; } interpose_##theirs \
    __attribute__((section("__DATA,__interpose"))) = {(const void *)mine, (const void *)theirs};
INTERPOSE(n_malloc, malloc)
INTERPOSE(n_calloc, calloc)
INTERPOSE(n_realloc, realloc)
INTERPOSE(n_free, free)
INTERPOSE(n_posix_memalign, posix_memalign)
INTERPOSE(n_aligned_alloc, aligned_alloc)
INTERPOSE(n_lock, pthread_mutex_lock)
#else
/* glibc's own entry points: dlsym would allocate before it could forward */
void *__libc_malloc(size_t);
void *__libc_calloc(size_t, size_t);
void *__libc_realloc(void *, size_t);
void __libc_free(void *);
void *__libc_memalign(size_t, size_t);

void *malloc(size_t n) { hit("malloc"); return __libc_malloc(n); }
void *calloc(size_t a, size_t b) { hit("calloc"); return __libc_calloc(a, b); }
void *realloc(void *p, size_t n) { hit("realloc"); return __libc_realloc(p, n); }
void free(void *p) { if (p) hit("free"); __libc_free(p); }
void *aligned_alloc(size_t a, size_t n) { hit("aligned_alloc"); return __libc_memalign(a, n); }
void *memalign(size_t a, size_t n) { hit("memalign"); return __libc_memalign(a, n); }
int posix_memalign(void **p, size_t a, size_t n) {
  hit("posix_memalign");
  void *q = __libc_memalign(a, n);
  if (!q) return 12; /* ENOMEM */
  *p = q;
  return 0;
}

static int (*real_lock)(pthread_mutex_t *);
__attribute__((constructor)) static void init(void) { real_lock = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_lock"); }
int pthread_mutex_lock(pthread_mutex_t *m) {
  hit("pthread_mutex_lock");
  return real_lock(m);
}
#endif
