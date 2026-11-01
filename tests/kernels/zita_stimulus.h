/* The input of tests/kernels/zita.c and the program that made its expected
   output: a click and noise on the left, a tone on the right, then their tail. */
#include <math.h>
enum { ZITA_FRAMES = 192000, ZITA_RATE = 48000 };
static void zita_stimulus(float *l, float *r) {
  unsigned s = 12345;
  for (int i = 0; i < ZITA_FRAMES; i++) l[i] = r[i] = 0;
  l[100] = 1;
  for (int i = 20000; i < 24000; i++) {
    s = (s * 1103515245u + 12345u) & 0x7fffffffu;
    l[i] = (float)(0.5 * (s / 1073741824.0 - 1.0));
  }
  for (int i = 30000; i < 36000; i++) r[i] = (float)(0.5 * sin(2 * 3.141592653589793 * 440.0 * (i - 30000) / 48000.0));
}
