/* Word frequencies over packed strings: words are built, joined with
   spaces, split again, merge-sorted as a linked list like the prelude's
   sortBy, and runs are counted. */
#include <string.h>

#include "arena.h"

typedef struct Str { size_t n; const char *s; } Str;
typedef struct Words { Str w; struct Words *next; } Words;
typedef struct Lists { Words *list; struct Lists *next; } Lists;

static const char *const vocab[] = {"the", "of", "and", "a", "to", "in", "is", "you", "that", "it",
                                    "he", "was", "for", "on", "are", "as", "with", "his", "they", "at"};

static long rnd(long s) { return (s * 1103515245 + 12345) % 2147483648; }

/* funk's String order: bytes, then length. */
static int cmp(Str a, Str b) {
  int c = memcmp(a.s, b.s, a.n < b.n ? a.n : b.n);
  return c ? c : (a.n > b.n) - (a.n < b.n);
}

static Words *wcons(Str w, Words *next) {
  Words *c = alloc(sizeof *c);
  c->w = w;
  c->next = next;
  return c;
}

static Lists *lcons(Words *l, Lists *next) {
  Lists *c = alloc(sizeof *c);
  c->list = l;
  c->next = next;
  return c;
}

static Words *merge(Words *xs, Words *ys) {
  Words *out = NULL, **tp = &out;
  while (xs && ys) {
    if (cmp(xs->w, ys->w) > 0) { *tp = wcons(ys->w, NULL); ys = ys->next; }
    else { *tp = wcons(xs->w, NULL); xs = xs->next; }
    tp = &(*tp)->next;
  }
  for (Words *r = xs ? xs : ys; r; r = r->next) { *tp = wcons(r->w, NULL); tp = &(*tp)->next; }
  return out;
}

int main(void) {
  long n = read_n(), s = 42;
  Str *ws = alloc((size_t)n * sizeof *ws + 1);
  size_t total = 0;
  for (long i = 0; i < n; i++, s = rnd(s)) {
    char buf[32];
    size_t vl = strlen(vocab[s % 20]);
    memcpy(buf, vocab[s % 20], vl);
    int len = (int)(put_decimal(buf + vl, (s / 20) % 37) - buf);
    buf[len] = 0;
    char *p = alloc((size_t)len + 1);
    memcpy(p, buf, (size_t)len + 1);
    ws[i] = (Str){(size_t)len, p};
    total += (size_t)len + (i > 0);
  }
  char *text = alloc(total + 1), *q = text;
  for (long i = 0; i < n; i++) {
    if (i) *q++ = ' ';
    memcpy(q, ws[i].s, ws[i].n);
    q += ws[i].n;
  }
  Lists *xss = NULL, **tp = &xss;
  for (size_t i = 0; i < total;) {
    while (i < total && text[i] == ' ') i++;
    if (i == total) break;
    size_t j = i;
    while (j < total && text[j] != ' ') j++;
    char *w = alloc(j - i + 1);
    memcpy(w, text + i, j - i);
    w[j - i] = 0;
    *tp = lcons(wcons((Str){j - i, w}, NULL), NULL);
    tp = &(*tp)->next;
    i = j;
  }
  while (xss && xss->next) {
    Lists *out = NULL, **op = &out;
    for (Lists *p = xss; p; p = p->next ? p->next->next : NULL) {
      *op = lcons(p->next ? merge(p->list, p->next->list) : p->list, NULL);
      op = &(*op)->next;
    }
    xss = out;
  }
  long distinct = 0, best = 0, k = 0;
  Str cur = {0, ""};
  for (Words *w = xss ? xss->list : NULL; w; w = w->next) {
    if (distinct && cmp(w->w, cur) == 0) { k++; continue; }
    if (k > best) best = k;
    cur = w->w;
    k = 1;
    distinct++;
  }
  if (k > best) best = k;
  printf("(%ld,%ld)\n", distinct, best);
}
