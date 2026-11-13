/* Runs exact.fk's k_exact on a product that a fused multiply-add would not
   round: (1 + 2^-12)^2 - (1 + 2^-11) is 2^-24 fused, 0 rounded first. */
#include <dlfcn.h>
#include <stdio.h>

typedef void (*Kernel)(void *state, float **in, float *out, int n, void *cx);
enum { N = 64 };

int main(int argc, char **argv) {
  void *lib = argc > 1 ? dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) : NULL;
  Kernel k = lib ? (Kernel)dlsym(lib, "k_exact") : NULL;
  if (!k) {
    fprintf(stderr, "no k_exact\n");
    return 1;
  }
  float a[N], b[N], c[N], out[N];
  for (int i = 0; i < N; i++) a[i] = b[i] = 1.0f + 0x1p-12f, c[i] = -(1.0f + 0x1p-11f);
  float *in[3] = {a, b, c};
  k(NULL, in, out, N, NULL);
  int fused = 0;
  for (int i = 0; i < N; i++) fused += out[i] != 0.0f;
  printf("frames fused: %d of %d\n", fused, N);
  return 0;
}
