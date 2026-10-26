#include "fkt.h"

int fkt_scale(int x, float f) { return (int)((float)x * f); }
size_t fkt_twice(size_t n) { return 2 * n; }
unsigned fkt_wrap(unsigned x) { return x + 1; }
