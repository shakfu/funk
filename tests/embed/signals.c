/* A host's SIGSEGV handler installed before funk's still gets the faults
   that are not funk's (rt.c, pass_on): here, writes to a page the host
   protects and unprotects on a fault, from funk's thread and the host's.
   funk's own faults, the stack snapshot's (realtime.md), still work: run
   under FUNK_GCSTRESS=1, an evaluation collects throughout. After
   funk_main returns, the host's handler is installed again. */
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libfunk.h"

static char *page;
static size_t page_size;
static volatile sig_atomic_t hits;

static void host_fault(int sig, siginfo_t *si, void *ctx) {
  (void)ctx;
  if ((char *)si->si_addr >= page && (char *)si->si_addr < page + page_size) {
    mprotect(page, page_size, PROT_READ | PROT_WRITE);
    hits++;
    return;
  }
  signal(sig, SIG_DFL);
}

static void touch(void) {
  mprotect(page, page_size, PROT_NONE);
  page[0]++;
}

static void *other(void *arg) {
  (void)arg;
  touch();
  return NULL;
}

static int run(void *arg) {
  (void)arg;
  if (!funk_init(NULL, 0)) return 1;
  touch(); /* on funk's thread */
  char *shown;
  if (!funk_eval("sum (map (\\x -> length (reverse [1 .. x])) [1 .. 300])", "<test>", &shown)) return 1;
  printf("%s\n", shown);
  free(shown);
  pthread_t t;
  pthread_create(&t, NULL, other, NULL);
  pthread_join(t, NULL);
  touch();
  printf("host handler: %d faults, page %d\n", (int)hits, page[0]);
  return 0;
}

int main(void) {
  page_size = (size_t)sysconf(_SC_PAGESIZE);
  page = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  struct sigaction sa = {0};
  sa.sa_sigaction = host_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL); /* macOS reports a protected page as SIGBUS */
  int status = funk_main(run, NULL);
  struct sigaction now;
  sigaction(SIGSEGV, NULL, &now);
  printf("after funk_main: %s\n", now.sa_sigaction == host_fault ? "host handler" : "another handler");
  return status;
}
