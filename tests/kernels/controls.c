/* projects/funkgraph/controls.c: Tidal's OSC /ctrl into a host's controls,
   over loopback as a sender would send it, and MIDI CC's names. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "controls.h"

/* an OSC string, NUL and padding at d; its padded length */
static size_t put_string(unsigned char *d, const char *s) {
  size_t n = strlen(s) + 1, p = (n + 3) & ~(size_t)3;
  memset(d, 0, p);
  memcpy(d, s, n - 1);
  return p;
}

static size_t put_int(unsigned char *d, uint32_t x) {
  d[0] = x >> 24, d[1] = x >> 16, d[2] = x >> 8, d[3] = x;
  return 4;
}

static size_t put_float(unsigned char *d, float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return put_int(d, u);
}

/* /ctrl with tags ",XY" and the arguments a, b; its length */
static size_t ctrl(unsigned char *d, const char *tags, const char *sa, int ia, const char *sb, double xb) {
  size_t n = put_string(d, "/ctrl");
  n += put_string(d + n, tags);
  n += tags[1] == 's' ? put_string(d + n, sa) : put_int(d + n, (uint32_t)ia);
  if (tags[2] == 's') n += put_string(d + n, sb);
  else if (tags[2] == 'i') n += put_int(d + n, (uint32_t)(int)xb);
  else n += put_float(d + n, (float)xb);
  return n;
}

static void show(const funk_controls *t) {
  for (long i = 0; i < t->n; i++) {
    const funk_control *c = &t->e[i];
    if (c->kind == 0) printf(" %s=%g", c->name, c->num.f);
    else if (c->kind == 1) printf(" %s=%lld", c->name, (long long)c->num.i);
    else printf(" %s='%s'", c->name, c->text);
  }
  printf("\n");
}

int main(void) {
  int port = 57340 + getpid() % 500, fd = funk_osc_open(port);
  if (fd < 0) return printf("cannot bind %d\n", port), 1;
  printf("a second socket on the port: %d\n", funk_osc_open(port));
  int out = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  unsigned char d[512], b[512];
  size_t n;
  funk_controls t = {0};
  printf("nothing waiting: %d\n", funk_osc_poll(fd, &t));
  /* a float, an int name, an int value, a string; then malformed and other ones */
  n = ctrl(d, ",sf", "cutoff", 0, NULL, 0.25), sendto(out, d, n, 0, (struct sockaddr *)&a, sizeof a);
  n = ctrl(d, ",if", NULL, 21, NULL, 0.5), sendto(out, d, n, 0, (struct sockaddr *)&a, sizeof a);
  n = ctrl(d, ",si", "steps", 0, NULL, 7), sendto(out, d, n, 0, (struct sockaddr *)&a, sizeof a);
  n = ctrl(d, ",ss", "vowel", 0, "a", 0), sendto(out, d, n, 0, (struct sockaddr *)&a, sizeof a);
  n = ctrl(d, ",sf", "cutoff", 0, NULL, 0.75), sendto(out, d, n - 2, 0, (struct sockaddr *)&a, sizeof a); /* cut short */
  n = put_string(d, "/other"), n += put_string(d + n, ",f"), n += put_float(d + n, 1), sendto(out, d, n, 0, (struct sockaddr *)&a, sizeof a);
  usleep(20000);
  printf("set: %d;", funk_osc_poll(fd, &t));
  show(&t);
  /* a bundle of two, one replacing a value */
  memcpy(b, "#bundle\0", 8), memset(b + 8, 0, 8);
  size_t m = 16;
  n = ctrl(d, ",sf", "cutoff", 0, NULL, 1.5), m += put_int(b + m, (uint32_t)n), memcpy(b + m, d, n), m += n;
  n = ctrl(d, ",ss", "s", 0, "bd", 0), m += put_int(b + m, (uint32_t)n), memcpy(b + m, d, n), m += n;
  sendto(out, b, m, 0, (struct sockaddr *)&a, sizeof a);
  usleep(20000);
  printf("bundle: %d;", funk_osc_poll(fd, &t));
  show(&t);
  /* MIDI CC: any channel's and its own, 0 to 1 */
  funk_controls cc = {0};
  funk_control_cc(&cc, 0, 21, 127);
  funk_control_cc(&cc, 9, 21, 0);
  printf("cc:");
  show(&cc);
  funk_osc_close(fd);
  close(out);
  return 0;
}
