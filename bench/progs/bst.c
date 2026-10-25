/* A persistent binary search tree: insert copies the path, as funk's
   constructors do; lookups walk it. */
#include "arena.h"

typedef struct Node { struct Node *l; long k, v; struct Node *r; } Node;

static Node *node(Node *l, long k, long v, Node *r) {
  Node *t = alloc(sizeof *t);
  *t = (Node){l, k, v, r};
  return t;
}

static Node *insert(long k, long v, Node *t) {
  if (!t) return node(NULL, k, v, NULL);
  if (k < t->k) return node(insert(k, v, t->l), t->k, t->v, t->r);
  if (k > t->k) return node(t->l, t->k, t->v, insert(k, v, t->r));
  return node(t->l, k, v, t->r);
}

static long find(long k, Node *t) {
  while (t) {
    if (k < t->k) t = t->l;
    else if (k > t->k) t = t->r;
    else return t->v;
  }
  return 0;
}

static long rnd(long s) { return (s * 1103515245 + 12345) % 2147483648; }

int main(void) {
  long n = read_n(), s = 42, acc = 0;
  Node *t = NULL;
  for (long i = n; i > 0; i--, s = rnd(s)) t = insert(s % 1000000, i, t);
  s = 7;
  for (long i = n; i > 0; i--, s = rnd(s)) acc += find(s % 1000000, t);
  printf("%ld\n", acc);
}
