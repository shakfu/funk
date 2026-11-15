/* name.h -- control names built on the audio thread, where libc's
   snprintf has no guarantee of not allocating or locking. */
#ifndef FUNK_NAME_H
#define FUNK_NAME_H

#include <stdbool.h>
#include <string.h>

typedef struct {
  char *at, *end; /* end: the last byte, kept for the terminator */
  bool ok;        /* false once cut short */
} Text;

static inline Text name_in(char *buf, size_t cap) { return (Text){buf, buf + cap - 1, true}; }

static inline void name_add(Text *n, const char *s, size_t len) {
  size_t room = (size_t)(n->end - n->at);
  if (len > room) len = room, n->ok = false;
  memcpy(n->at, s, len);
  n->at += len;
}

static inline void name_str(Text *n, const char *s) { name_add(n, s, strlen(s)); }

static inline void name_int(Text *n, int k) {
  char d[12], *p = d + sizeof d;
  unsigned u = k < 0 ? 0u - (unsigned)k : (unsigned)k;
  do *--p = (char)('0' + u % 10); while (u /= 10);
  if (k < 0) *--p = '-';
  name_add(n, p, (size_t)(d + sizeof d - p));
}

/* terminates the name; false if it was cut short */
static inline bool name_done(Text *n) {
  *n->at = 0;
  return n->ok;
}

#endif
