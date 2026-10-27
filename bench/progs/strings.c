/* Packed strings, as funk's String: each number is formatted into its own
   string, the strings are joined with spaces into one buffer (unwords),
   and the buffer is split into a list of copied words (words). */
#include <string.h>

#include "arena.h"

typedef struct Str { size_t n; char *s; } Str;
typedef struct Words { Str w; struct Words *next; } Words;

static Str show(long i) {
  char buf[32];
  int len = (int)(put_decimal(buf, i) - buf);
  buf[len] = 0;
  char *s = alloc((size_t)len + 1);
  memcpy(s, buf, (size_t)len + 1);
  return (Str){(size_t)len, s};
}

int main(void) {
  long n = read_n();
  Str *parts = alloc((size_t)n * sizeof *parts);
  size_t total = n ? (size_t)n - 1 : 0;
  for (long i = 0; i < n; i++) total += (parts[i] = show(i + 1)).n;
  char *s = alloc(total + 1), *q = s;
  for (long i = 0; i < n; i++) {
    if (i) *q++ = ' ';
    memcpy(q, parts[i].s, parts[i].n);
    q += parts[i].n;
  }
  *q = 0;
  Words *head = NULL, **dst = &head;
  long words = 0;
  for (size_t i = 0; i < total;) {
    while (i < total && s[i] == ' ') i++;
    if (i == total) break;
    size_t j = i;
    while (j < total && s[j] != ' ') j++;
    Words *w = alloc(sizeof *w);
    w->w.n = j - i;
    w->w.s = alloc(j - i + 1);
    memcpy(w->w.s, s + i, j - i);
    w->w.s[j - i] = 0;
    w->next = NULL;
    *dst = w;
    dst = &w->next;
    words++;
    i = j;
  }
  printf("%ld\n", (long)total + words);
}
