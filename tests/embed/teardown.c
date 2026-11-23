/* teardown FILE [CC_ARGS]: a session freed while another thread queues its
   callbacks (rt.c, fk_stop). FILE starts tests/ffi/fkt.h's sender and
   returns. Later claims find the session gone while the sender runs on for
   300 ms. Then a new session takes the freed id: without the generation in
   the StablePtr, the sender's claims reached it before its ring existed, and
   most runs crashed. embed/inflight tests the wait for a claim not yet
   published. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libfunk.h"

static int run_file(funk *f, void *arg) { return !funk_run_file(f, arg); }

static int run_expr(funk *f, void *arg) {
  (void)arg;
  char *shown;
  if (!funk_eval(f, "1 + 1", "<teardown>", &shown)) return 1;
  printf("next session: %s\n", shown);
  free(shown);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  funk_config c = {.cc_args = (const char *const *)argv + 2, .ncc_args = argc - 2};
  funk *f = funk_new(&c);
  if (!f) return 1;
  int status = funk_run(f, run_file, argv[1]);
  funk_free(f);
  if (status) return status;
  printf("freed while sending\n");
  usleep(300 * 1000);
  if (!(f = funk_new(&c))) return 1;
  status = funk_run(f, run_expr, NULL);
  funk_free(f);
  return status;
}
