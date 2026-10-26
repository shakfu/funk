/* A library outside the default search paths, for tests/run.sh: funk run
   and funk build find it with -I, -L and -l. */
#include <stddef.h>

int fkt_scale(int x, float f);
size_t fkt_twice(size_t n);
unsigned fkt_wrap(unsigned x);
#define FKT_LIMIT 255u
