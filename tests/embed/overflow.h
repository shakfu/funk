/* overflow.h -- functions of tests/embed/overflow.c, given to funk_provide */
long overflow_deep(long n);
long overflow_libc(long n);
void overflow_each(void *ctx, void (*fn)(void *, int));
