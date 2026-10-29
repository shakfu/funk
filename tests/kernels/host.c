/* Loads a library of kernels (dsp.fk) and runs a chain of them: saw into
   a one-pole filter, times a sine, clipped; and a voice, which is a saw and
   a filter in one. Prints each stage. */
#include <dlfcn.h>
#include <stdio.h>

typedef void (*Kernel)(void *state, float **in, float *out, int n);
enum { N = 8 };

static Kernel get(void *lib, const char *name) {
  Kernel k = (Kernel)dlsym(lib, name);
  if (!k) fprintf(stderr, "no %s\n", name);
  return k;
}

static void show(const char *what, const float *x) {
  printf("%-8s", what);
  for (int i = 0; i < N; i++) printf(" %9.6f", x[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  void *lib = argc > 1 ? dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) : NULL;
  if (!lib) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  Kernel saw = get(lib, "k_saw"), sine = get(lib, "k_sine"), pole = get(lib, "k_onepole"), mul = get(lib, "k_mul"),
         clip = get(lib, "k_clip"), voice = get(lib, "k_voice");
  if (!saw || !sine || !pole || !mul || !clip || !voice) return 1;
  double osc1[2] = {0.0, 1000.0}, osc2[2] = {0.25, 500.0}, lp[2] = {0.0, 0.5};
  double v[4] = {0.0, 1000.0, 0.0, 0.5}; /* Voice: osc then lp, as osc1 and lp */
  float a[N], b[N], c[N], d[N], e[N], f[N];
  float *in_a[1] = {a}, *in_bc[2] = {b, c}, *in_d[1] = {d};
  for (int block = 0; block < 2; block++) { /* state carries over between blocks */
    saw(osc1, NULL, a, N);
    pole(lp, in_a, b, N);
    sine(osc2, NULL, c, N);
    mul(NULL, in_bc, d, N);
    clip(NULL, in_d, e, N);
    voice(v, NULL, f, N);
    printf("block %d\n", block);
    show("saw", a);
    show("onepole", b);
    show("sine", c);
    show("mul", d);
    show("clip", e);
    show("voice", f); /* equals onepole */
  }
  return 0;
}
