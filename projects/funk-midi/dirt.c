/* dirt.c -- SuperDirt's /dirt/play over OSC (dirt.h, docs/dev/dirt.md). */
#define _DEFAULT_SOURCE
#include "dirt.h"

#include <netdb.h>
#include <poll.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int64_t rd_int(const unsigned char *p) {
  int64_t x;
  memcpy(&x, p, 8);
  return x;
}

static double rd_float(const unsigned char *p) {
  double x;
  memcpy(&x, p, 8);
  return x;
}

/* a writeDirtWith string at p: its bytes and length; the size it takes */
static size_t rd_str(const unsigned char *p, const unsigned char *end, const char **s, size_t *len) {
  if (end - p < 8) return 0;
  int64_t n = rd_int(p);
  size_t size = 8 + 8 * (size_t)((n + 7) / 8);
  if (n < 0 || (size_t)(end - p) < size) return 0;
  *s = (const char *)p + 8;
  *len = (size_t)n;
  return size;
}

typedef struct {
  const char *name;
  size_t nlen;
  char tag; /* f, i, s */
  double f;
  int64_t i;
  const char *s;
  size_t slen;
} Arg;

/* the pair at p into a; its size, 0 if malformed */
static size_t rd_pair(const unsigned char *p, const unsigned char *end, Arg *a) {
  if (end - p < 8) return 0;
  int64_t kind = rd_int(p);
  size_t k = rd_str(p + 8, end, &a->name, &a->nlen);
  if (!k) return 0;
  const unsigned char *v = p + 8 + k;
  if (kind == 2) {
    size_t m = rd_str(v, end, &a->s, &a->slen);
    a->tag = 's';
    return m ? 8 + k + m : 0;
  }
  if (end - v < 8 || (kind != 0 && kind != 1)) return 0;
  a->tag = kind ? 'i' : 'f';
  if (kind) a->i = rd_int(v);
  else a->f = rd_float(v);
  return 8 + k + 8;
}

int dirt_next(const unsigned char *p, const unsigned char *end, dirt_event *e) {
  if (end - p < 8 || rd_int(p) == -1 || end - p < 32) return 0;
  e->start = rd_int(p);
  e->dur = rd_int(p + 8);
  e->cycle = rd_float(p + 16);
  e->n = rd_int(p + 24);
  e->pairs = p + 32;
  const unsigned char *q = e->pairs;
  for (int64_t i = 0; i < e->n; i++) {
    Arg a;
    size_t k = rd_pair(q, end, &a);
    if (!k) return 0;
    q += k;
  }
  e->next = q;
  return 1;
}

static int same(const Arg *a, const char *name) { return a->nlen == strlen(name) && !memcmp(a->name, name, a->nlen); }

double dirt_float(const dirt_event *e, const char *name, double dflt) {
  const unsigned char *q = e->pairs;
  for (int64_t i = 0; i < e->n; i++) {
    Arg a;
    q += rd_pair(q, e->next, &a);
    if (same(&a, name)) return a.tag == 'f' ? a.f : a.tag == 'i' ? (double)a.i : dflt;
  }
  return dflt;
}

/* by name, as Haskell orders Strings: bytes, then length */
static int by_name(const void *x, const void *y) {
  const Arg *a = x, *b = y;
  size_t n = a->nlen < b->nlen ? a->nlen : b->nlen;
  int c = memcmp(a->name, b->name, n);
  return c ? c : (a->nlen > b->nlen) - (a->nlen < b->nlen);
}

typedef struct {
  unsigned char *p;
  size_t n, cap;
  int full;
} Out;

static void put(Out *o, const void *d, size_t n) {
  if (o->n + n > o->cap) {
    o->full = 1;
    return;
  }
  memcpy(o->p + o->n, d, n);
  o->n += n;
}

static void put32(Out *o, uint32_t x) {
  unsigned char b[4] = {(unsigned char)(x >> 24), (unsigned char)(x >> 16), (unsigned char)(x >> 8), (unsigned char)x};
  put(o, b, 4);
}

/* an OSC string: its bytes, then 1 to 4 NULs to a multiple of 4 */
static void put_str(Out *o, const char *s, size_t n) {
  static const unsigned char zeros[4] = {0};
  put(o, s, n);
  put(o, zeros, 4 - n % 4);
}

size_t dirt_bundle(const dirt_event *e, uint64_t tag, double cps, double delta, unsigned char *out, size_t cap) {
  size_t na = 0;
  Arg *as = malloc((size_t)(e->n + 3) * sizeof *as);
  if (!as) return 0;
  const unsigned char *q = e->pairs;
  for (int64_t i = 0; i < e->n; i++) {
    Arg a;
    q += rd_pair(q, e->next, &a);
    as[na++] = a;
  }
  /* Tidal's Map.union playmap extra: a control of the pattern's own keeps its value */
  const char *extra[3] = {"cps", "cycle", "delta"};
  double val[3] = {cps, e->cycle, delta};
  for (int k = 0; k < 3; k++) {
    int has = 0;
    for (size_t i = 0; i < na; i++) has |= same(&as[i], extra[k]);
    if (!has) as[na++] = (Arg){.name = extra[k], .nlen = strlen(extra[k]), .tag = 'f', .f = val[k]};
  }
  qsort(as, na, sizeof *as, by_name);

  Out o = {out, 0, cap, 0};
  put(&o, "#bundle", 8);
  put32(&o, (uint32_t)(tag >> 32));
  put32(&o, (uint32_t)tag);
  size_t at = o.n;
  put32(&o, 0); /* the message's size, below */
  size_t m0 = o.n;
  put_str(&o, "/dirt/play", 10);
  char *tags = malloc(2 * na + 2);
  if (!tags) {
    free(as);
    return 0;
  }
  tags[0] = ',';
  for (size_t i = 0; i < na; i++) tags[1 + 2 * i] = 's', tags[2 + 2 * i] = as[i].tag;
  put_str(&o, tags, 2 * na + 1);
  free(tags);
  for (size_t i = 0; i < na; i++) {
    put_str(&o, as[i].name, as[i].nlen);
    if (as[i].tag == 's') put_str(&o, as[i].s, as[i].slen);
    else if (as[i].tag == 'i') put32(&o, (uint32_t)as[i].i); /* Tidal's int32: wraps */
    else {
      float f = (float)as[i].f;
      uint32_t bits;
      memcpy(&bits, &f, 4);
      put32(&o, bits);
    }
  }
  free(as);
  if (o.full) return 0;
  uint32_t size = (uint32_t)(o.n - m0);
  unsigned char b[4] = {(unsigned char)(size >> 24), (unsigned char)(size >> 16), (unsigned char)(size >> 8), (unsigned char)size};
  memcpy(out + at, b, 4);
  return o.n;
}

uint64_t dirt_ntp(int64_t us) {
  int64_t sec = us / 1000000, rem = us % 1000000;
  if (rem < 0) sec--, rem += 1000000;
  return (uint64_t)(sec + 2208988800LL) << 32 | (uint64_t)(((uint64_t)rem << 32) / 1000000);
}

static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* an OSC string at *at, before n: its start, and *at past its padding */
static const char *get_str(const unsigned char *b, size_t n, size_t *at) {
  const char *s = (const char *)b + *at;
  size_t len = strnlen(s, n - *at);
  if (*at + len >= n) return NULL;
  *at += len + 4 - len % 4;
  return s;
}

void dirt_print(FILE *f, const unsigned char *b, size_t n, double base_ms) {
  if (n < 20 || memcmp(b, "#bundle", 8)) {
    fputs("dirt: not a bundle\n", f);
    return;
  }
  uint64_t tag = (uint64_t)get32(b + 8) << 32 | get32(b + 12);
  double ms = (double)(tag >> 32) * 1000 + (double)(uint32_t)tag * 1000 / 4294967296.0 - base_ms;
  size_t size = get32(b + 16), at = 20, end = 20 + size;
  if (end > n) end = n;
  const char *addr = get_str(b, end, &at), *tags = addr ? get_str(b, end, &at) : NULL;
  if (!tags || tags[0] != ',') {
    fputs("dirt: a malformed message\n", f);
    return;
  }
  fprintf(f, "%9.3f dirt %s", ms, addr);
  for (const char *t = tags + 1; *t; t++) {
    if (*t == 's') {
      const char *s = get_str(b, end, &at);
      if (!s) break;
      fprintf(f, (t - tags) % 2 ? " %s=" : "s:%s", s);
    } else if (at + 4 <= end) {
      uint32_t x = get32(b + at);
      at += 4;
      if (*t == 'i') fprintf(f, "i:%d", (int32_t)x);
      else fprintf(f, "f:%08x", x);
    }
  }
  fputc('\n', f);
}

int dirt_open(const char *host, int port) {
  char ps[16];
  snprintf(ps, sizeof ps, "%d", port);
  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *ai;
  if (getaddrinfo(host, ps, &hints, &ai)) return -1;
  int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen)) {
    close(fd);
    fd = -1;
  }
  freeaddrinfo(ai);
  return fd;
}

static int64_t ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the address of a datagram's message, or of its bundle's first */
static const char *address(const unsigned char *b, size_t n) {
  size_t at = 0;
  if (n >= 20 && !memcmp(b, "#bundle", 8)) {
    size_t size = get32(b + 16);
    if (size > n - 20) return NULL;
    b += 20;
    n = size;
  }
  return n ? get_str(b, n, &at) : NULL;
}

void dirt_handshake(int fd, volatile int *stop, volatile int *connected, FILE *log) {
  static const unsigned char hs[] = "/dirt/handshake\0,\0\0"; /* 16 + 4 bytes */
  int waits = 0;
  while (!*stop) {
    if (!*connected || waits < 0) {
      (void)send(fd, hs, 20, 0);
      waits = waits < 0 ? 0 : waits;
    }
    /* up to 2 s of replies, as Tidal's recvMessagesTimeout 2 */
    int64_t deadline = ms_now() + 2000;
    int got = 0;
    while (!*stop) {
      int64_t left = deadline - ms_now();
      if (left <= 0) break;
      struct pollfd p = {fd, POLLIN, 0};
      if (poll(&p, 1, (int)left) <= 0) break;
      unsigned char b[2048];
      ssize_t k = recv(fd, b, sizeof b, 0);
      if (k < 0) { /* nothing on the port: the OS refuses; wait out the 2 s */
        int64_t rest = deadline - ms_now();
        if (rest > 0) usleep((useconds_t)(rest * 1000));
        break;
      }
      const char *a = address(b, (size_t)k);
      if (!a) continue;
      got = 1;
      if (!strcmp(a, "/dirt/hello")) waits = -1; /* SuperDirt restarted: shake again */
      else if (!strcmp(a, "/dirt/handshake/reply")) {
        if (!*connected) fputs("Connected to SuperDirt.\n", log);
        fflush(log);
        *connected = 1;
      }
      if (waits < 0) break;
    }
    if (!got && !*connected) {
      if (waits == 0) {
        fputs("Waiting for SuperDirt (v.1.7.2 or higher)..\n", log);
        fflush(log);
      }
      waits++;
    }
  }
}
