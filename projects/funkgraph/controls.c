/* controls.c -- Tidal's OSC /ctrl into a host's controls; see controls.h. */
#include "controls.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int funk_osc_open(int port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (port <= 0 || port > 65535 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void funk_osc_close(int fd) {
  if (fd >= 0) close(fd);
}

/* An OSC string at *at, padded to 4 bytes; NULL past the end */
static const char *osc_string(const unsigned char *d, size_t n, size_t *at) {
  size_t i = *at;
  while (i < n && d[i]) i++;
  if (i >= n) return NULL;
  const char *s = (const char *)d + *at;
  *at = (i + 4) & ~(size_t)3;
  return *at <= n ? s : NULL;
}

static int osc_int(const unsigned char *d) { return (int)((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]); }

static float osc_float(const unsigned char *d) {
  uint32_t u = (uint32_t)osc_int(d);
  float f;
  memcpy(&f, &u, sizeof f);
  return f;
}

/* one argument of type c at *at: as text, or a number in *x; 0 if malformed */
static int osc_arg(char c, const unsigned char *d, size_t n, size_t *at, char *text, size_t cap, double *x, int *kind) {
  if (c == 's') {
    const char *s = osc_string(d, n, at);
    if (!s) return 0;
    snprintf(text, cap, "%s", s);
    *kind = 2;
  } else if (c == 'i' || c == 'f') {
    if (*at + 4 > n) return 0;
    *x = c == 'i' ? (double)osc_int(d + *at) : (double)osc_float(d + *at);
    *kind = c == 'i' ? 1 : 0;
    *at += 4;
  } else return 0;
  return 1;
}

static int osc_message(const unsigned char *d, size_t n, funk_controls *t) {
  size_t at = 0;
  const char *addr = osc_string(d, n, &at), *tags = addr ? osc_string(d, n, &at) : NULL;
  if (!tags || strcmp(addr, "/ctrl") || strlen(tags) != 3 || tags[0] != ',') return 0;
  char name[64], text[64];
  double k = 0, v = 0;
  int kk, vk;
  if (!osc_arg(tags[1], d, n, &at, name, sizeof name, &k, &kk) || !osc_arg(tags[2], d, n, &at, text, sizeof text, &v, &vk)) return 0;
  if (kk == 0) return 0; /* a name is a string or an int */
  if (kk == 1) snprintf(name, sizeof name, "%d", (int)k);
  if (vk == 2) funk_control_text(t, name, text);
  else if (vk == 1) funk_control_int(t, name, (int64_t)v);
  else funk_control_float(t, name, v);
  return 1;
}

int funk_osc_take(const unsigned char *d, size_t n, funk_controls *t) {
  if (n >= 16 && !memcmp(d, "#bundle", 8)) { /* "#bundle", a time tag, then sized elements */
    int count = 0;
    for (size_t at = 16; at + 4 <= n;) {
      size_t size = (size_t)(uint32_t)osc_int(d + at);
      at += 4;
      if (size > n - at) break;
      count += funk_osc_take(d + at, size, t);
      at += size;
    }
    return count;
  }
  return osc_message(d, n, t);
}

int funk_osc_poll(int fd, funk_controls *t) {
  unsigned char d[2048];
  int count = 0;
  ssize_t n;
  while (fd >= 0 && (n = recv(fd, d, sizeof d, 0)) >= 0) count += funk_osc_take(d, (size_t)n, t);
  return count;
}
