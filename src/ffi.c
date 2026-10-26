/* ffi.c -- foreign imports (ffi.md). ffi_glue writes one C wrapper per
   import, with funk's calling convention. Compiled programs include the
   wrappers; the interpreter compiles them with $CC into a shared library,
   cached by the hash of its source, and loads it. Both backends therefore
   run the same conversions. */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <limits.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "funk.h"

Strs ffi_libs, ffi_cc_args;

/* The funk types that pass to C, and how each is held: 'i' as an Int,
   'f' as a Float, 'c' Char, 'b' Bool, 'v' (), 's' String, 'p' Ptr, FunPtr
   or StablePtr, 'L' CStringLen. The C
   types wrap to their width in their Num instances, so a value always fits. */
const FfiType ffi_types[] = {
    {"Int", "int64_t", 'i', 8},
    {"Float", "double", 'f', 8},
    {"Char", "char", 'c', 1},
    {"Bool", "bool", 'b', 1},
    {"()", "void", 'v', 0},
    {"String", "const char *", 's', 0},
    {"CChar", "char", 'i', CHAR_MIN < 0 ? 1 : -1},
    {"CBool", "bool", 'i', -1}, /* 0 or 1: any other value would be undefined in C */
    {"CInt", "int", 'i', (int)sizeof(int)},
    {"CUInt", "unsigned int", 'i', -(int)sizeof(unsigned)},
    {"CLong", "long", 'i', (int)sizeof(long)},
    {"CULong", "unsigned long", 'i', -(int)sizeof(unsigned long)},
    {"CLLong", "long long", 'i', (int)sizeof(long long)},
    {"CSize", "size_t", 'i', -(int)sizeof(size_t)},
    {"CFloat", "float", 'f', 4},
    {"CStringLen", "const char *", 'L', 0}, /* in a callback: a pointer and a size_t, copied into a String */
};
const int ffi_ntypes = sizeof ffi_types / sizeof ffi_types[0];

static const FfiType *ffi_type(Type *t) {
  static const FfiType ptr = {"Ptr", "void *", 'p', (int)sizeof(void *)};
  t = repr(t);
  if (t->k != TY_CON) return NULL;
  if (t->n == 1 && (!strcmp(t->name, "Ptr") || !strcmp(t->name, "FunPtr") || !strcmp(t->name, "StablePtr"))) return &ptr;
  for (int i = 0; i < ffi_ntypes && !t->n; i++)
    if (!strcmp(t->name, ffi_types[i].name)) return &ffi_types[i];
  return NULL;
}

int ffi_kind(Type *t) {
  const FfiType *f = ffi_type(t);
  return f ? f->kind : 0;
}

/* The C type a funk type passes as, or NULL. () is `void`: a result, or
   the only parameter of a C function without arguments. */
const char *ffi_param_type(Type *t) {
  const FfiType *f = ffi_type(t);
  return f ? f->ctype : NULL;
}

/* v as a C value of t's type; the representation keeps it in range. */
static const char *unbox(Type *t, const char *v) {
  const FfiType *f = ffi_type(t);
  switch (f->kind) {
  case 'i': return xsprintf("(%s)FK_INT(%s)", f->ctype, v);
  case 'f': return xsprintf("(%s)FK_FLOATV(%s)", f->ctype, v);
  case 'c': return xsprintf("(char)FK_CHAR(%s)", v);
  case 's': return xsprintf("fk_cstr(%s)", v);
  case 'p': return xsprintf("FK_PTRV(%s)", v);
  default: return xsprintf("FK_ISTRUE(%s)", v);
  }
}

static const char *box(Type *t, const char *x) {
  switch (ffi_kind(t)) {
  case 'i': return xsprintf("FK_MKINT((int64_t)(%s))", x);
  case 'f': return xsprintf("fk_float((double)(%s))", x);
  case 'c': return xsprintf("FK_MKCHAR((unsigned char)(%s))", x);
  case 'b': return xsprintf("FK_BOOL((%s) != 0)", x);
  case 'p': return xsprintf("fk_ptr(%s)", x);
  default: return "fk_unit";
  }
}

/* A trampoline: the C function behind a "callback" or "queue" import, and
   the wrapper that gives its address. Its first parameter is the context,
   a StablePtr to the funk closure, which takes the other arguments, or ()
   if there are none. A callback runs the closure now, on funk's thread; a
   runtime error stops here and is raised again when the foreign call
   returns, so it never unwinds C frames. A queued one copies its arguments
   into an event, which waitEvent runs. */
static void trampoline(SB *b, ForeignDecl *f, bool exported) {
  Type *t = repr(repr(globals.a[f->gidx].type)->args[0]), *r;
  SB params = {0}, fields = {0}, fill = {0}, frees = {0}, args = {0};
  bool q = f->cb == 'q';
  int n = 0, id = f->gidx;
  sb_puts(&params, "void *ctx");
  for (r = repr(t->args[1]); r->k == TY_CON && !strcmp(r->name, "->"); r = repr(r->args[1]), n++) {
    Type *a = r->args[0];
    int k = ffi_kind(a);
    const char *x = xsprintf("x%d", n), *v = q ? xsprintf("e->x%d", n) : x;
    if (k == 'L') sb_printf(&params, ", const char *%s, size_t %sn", x, x);
    else sb_printf(&params, ", %s %s", ffi_param_type(a), x);
    if (k == 'L') sb_printf(&args, "%sfk_str(%s ? %s : \"\", %sn)", n ? ", " : "", v, v, v);
    else if (k == 's') sb_printf(&args, "%sfk_cstr_result(%s, \"%s\")", n ? ", " : "", v, f->name);
    else sb_printf(&args, "%s%s", n ? ", " : "", box(a, v));
    if (!q) continue;
    if (k == 'L') {
      sb_printf(&fields, " char *%s; size_t %sn;", x, x);
      sb_printf(&fill, "  e->%s = fk_memdup(%s, %sn);\n  e->%sn = %sn;\n", x, x, x, x, x);
    } else if (k == 's') {
      sb_printf(&fields, " char *%s;", x);
      sb_printf(&fill, "  e->%s = %s ? fk_memdup(%s, strlen(%s)) : NULL;\n", x, x, x, x);
    } else {
      sb_printf(&fields, " %s %s;", ffi_param_type(a), x);
      sb_printf(&fill, "  e->%s = %s;\n", x, x);
    }
    if (k == 'L' || k == 's') sb_printf(&frees, "  free(e->%s);\n", x);
  }
  if (!n) sb_puts(&args, "fk_unit");
  int na = n ? n : 1;
  const char *rt = ffi_param_type(r);
  if (q) {
    sb_printf(b, "struct fk_ev_%d { FkEvent h;%s };\n", id, fields.s);
    sb_printf(b, "static void fk_evrun_%d(FkEvent *h) {\n  struct fk_ev_%d *e = (struct fk_ev_%d *)h;\n  V a[%d] = {%s};\n%s"
                 "  V f = fk_stable_deref(h->ctx);\n  free(e);\n  fk_apply(f, %d, a);\n}\n",
              id, id, id, na, args.s, frees.s, na);
    sb_printf(b, "static void fk_cb_%d(%s) { /* %s */\n  struct fk_ev_%d *e = calloc(1, sizeof *e);\n"
                 "  if (!e) fk_callback_oom(\"%s\");\n  e->h.run = fk_evrun_%d;\n  e->h.ctx = ctx;\n%s  fk_event_push(&e->h);\n}\n",
              id, params.s, f->name, id, f->name, id, fill.s);
  } else {
    int rk = ffi_kind(r);
    const char *ret = rk == 'v' ? "return;" : "return 0;"; /* C gets 0 while a failure is pending */
    sb_printf(b, "static %s fk_cb_%d(%s) { /* %s */\n  if (!fk_on_funk_thread()) fk_callback_thread(\"%s\");\n  if (fk_cb_failed) %s\n"
                 "  jmp_buf jb, *saved = fk_error_jmp;\n  if (setjmp(jb)) {\n    fk_error_jmp = saved;\n    fk_cb_failed = 1;\n    %s\n  }\n"
                 "  fk_error_jmp = &jb;\n  V a[%d] = {%s};\n  V r = fk_apply(fk_stable_deref(ctx), %d, a);\n  fk_error_jmp = saved;\n",
              rt, id, params.s, f->name, f->name, ret, ret, na, args.s, na);
    if (rk == 'v') sb_puts(b, "  (void)r;\n}\n");
    else sb_printf(b, "  return %s;\n}\n", unbox(r, "r"));
  }
  sb_printf(b, "%sV fk_ffi_%d(V self, V *a) { (void)self; (void)a; return fk_ptr((void *)fk_cb_%d); }\n", exported ? "" : "static ", id, id);
}

/* One wrapper per import. Arguments are locals of their declared C types,
   so passing them converts implicitly, and the pragmas make every
   conversion that can change a value an error: a type that differs from
   the C declaration in width, sign or kind does not compile. Widening is
   allowed, and a Ptr converts to any object pointer, as in Haskell. */
char *ffi_glue(ForeignDecl **fs, int n, bool exported) {
  SB b = {0};
  sb_puts(&b, "\n/* ---- foreign imports ---- */\n#include <stdlib.h>\n");
  for (int i = 0; i < n; i++) {
    bool seen = false;
    for (int j = 0; j < i; j++) seen |= fs[j]->header == fs[i]->header;
    if (!seen && fs[i]->header) sb_printf(&b, "#include <%s>\n", fs[i]->header);
  }
  sb_puts(&b, "#pragma GCC diagnostic push\n"
              "#pragma GCC diagnostic error \"-Wconversion\"\n"
              "#pragma GCC diagnostic error \"-Wsign-conversion\"\n"
              "#pragma GCC diagnostic error \"-Wint-conversion\"\n"
              "#pragma GCC diagnostic error \"-Wincompatible-pointer-types\"\n"
              "#ifdef __clang__\n#pragma GCC diagnostic error \"-Wincompatible-pointer-types-discards-qualifiers\"\n"
              "#else\n#pragma GCC diagnostic error \"-Wdiscarded-qualifiers\"\n#endif\n");
  for (int i = 0; i < n; i++) {
    ForeignDecl *f = fs[i];
    if (f->cb) {
      trampoline(&b, f, exported);
      continue;
    }
    SB body = {0}, call = {0};
    Type *t = repr(globals.a[f->gidx].type);
    sb_printf(&call, "%s%s", f->cname, f->arity ? "(" : "");
    for (int k = 0; k < f->arity; k++, t = repr(t->args[1])) {
      if (ffi_kind(t->args[0]) == 'v') continue; /* f () */
      sb_printf(&body, "  %s x%d = %s;\n", ffi_param_type(t->args[0]), k, unbox(t->args[0], xsprintf("a[%d]", k)));
      sb_printf(&call, "%sx%d", k ? ", " : "", k);
    }
    if (f->arity) sb_puts(&call, ")");
    const char *what = xsprintf("/* %s :: %s, from %s */", f->name, type_str(globals.a[f->gidx].type), f->header);
    /* fk_cb_check: a direct callback that failed during the call */
    switch (ffi_kind(t)) {
    case 'v': sb_printf(&body, "  %s; %s\n  fk_cb_check();\n  return fk_unit;\n", call.s, what); break;
    case 's': sb_printf(&body, "  const void *r = %s; %s\n  fk_cb_check();\n  return fk_cstr_result(r, \"%s\");\n", call.s, what, f->cname); break;
    case 'p': sb_printf(&body, "  const void *r = %s; %s\n  fk_cb_check();\n  return fk_ptr(r);\n", call.s, what); break;
    default: sb_printf(&body, "  %s r = %s; %s\n  fk_cb_check();\n  return %s;\n", ffi_param_type(t), call.s, what, box(t, "r"));
    }
    sb_printf(&b, "%sV fk_ffi_%d(V self, V *a) {\n  (void)self; (void)a;\n%s}\n", exported ? "" : "static ", f->gidx, body.s);
  }
  sb_puts(&b, "#pragma GCC diagnostic pop\n");
  return b.s;
}

/* FNV-1a, for the cache name. */
static uint64_t hash(uint64_t h, const char *s) {
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
  return h;
}

static const char *cache_dir(void) {
  const char *x = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
  char *base = x && *x ? xsprintf("%s", x) : xsprintf("%s/.cache", home && *home ? home : "/tmp");
  const char *parts[] = {base, xsprintf("%s/funk", base), xsprintf("%s/funk/ffi", base)};
  for (int i = 0; i < 3; i++) mkdir(parts[i], 0755); /* existing directories are fine */
  return parts[2];
}

/* Deletes cache entries unused for 30 days; a hit refreshes its mtime. */
static void prune(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return;
  time_t old = time(NULL) - 30 * 24 * 3600;
  for (struct dirent *e; (e = readdir(d));) {
    char *p = xsprintf("%s/%s", dir, e->d_name);
    struct stat st;
    if (e->d_name[0] != '.' && stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_mtime < old) unlink(p);
  }
  closedir(d);
}

void ffi_load(ForeignDecl **fs, int n) {
  if (!n) return;
  Argv argv = cc_argv();
  char *src = xsprintf("%s%s", fk_rth_src, ffi_glue(fs, n, true));
  uint64_t h = hash(14695981039346656037ULL, src); /* the compiler and libraries too, below */
  for (int i = 0; i < argv.n; i++) h = hash(h, argv.a[i]);
  for (int i = 0; i < ffi_libs.n; i++) h = hash(h, ffi_libs.a[i]);
  for (int i = 0; i < ffi_cc_args.n; i++) h = hash(h, ffi_cc_args.a[i]);
  const char *dir = cache_dir();
  char *so = xsprintf("%s/%016llx.so", dir, (unsigned long long)h);
  if (access(so, R_OK) == 0) {
    utimes(so, NULL);
  } else {
    prune(dir);
    char *c = xsprintf("%s/%016llx.c", dir, (unsigned long long)h), *tmp = xsprintf("%s.%d", so, (int)getpid());
    FILE *f = fopen(c, "w");
    if (!f) fk_error("cannot write %s", c);
    fputs(src, f);
    fclose(f);
    const char *fixed[] = {"-O2", "-shared", "-fPIC",
#ifdef __APPLE__
                           "-undefined", "dynamic_lookup", /* the runtime's functions come from funk itself */
#endif
                           "-o", tmp, c};
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) VPUSH(argv, (char *)fixed[i]);
    for (int i = 0; i < ffi_cc_args.n; i++) VPUSH(argv, (char *)ffi_cc_args.a[i]);
    bool m = false; /* the linker warns about a library named twice */
    for (int i = 0; i < ffi_libs.n; i++) {
      VPUSH(argv, xsprintf("-l%s", ffi_libs.a[i]));
      m |= !strcmp(ffi_libs.a[i], "m");
    }
    if (!m) VPUSH(argv, "-lm");
    const char *cc = argv.a[0];
    if (spawn_wait(&argv) != 0) {
      unlink(tmp);
      fk_error("foreign imports failed to compile with %s; its messages are above, and a conversion error means an import's type differs from its C declaration (source: %s)", cc, c);
    }
    if (rename(tmp, so) != 0) fk_error("cannot write %s", so);
  }
  void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
  if (!lib) fk_error("cannot load foreign imports: %s", dlerror());
  for (int i = 0; i < n; i++) {
    char *sym = xsprintf("fk_ffi_%d", fs[i]->gidx);
    fs[i]->code = (FkCode)dlsym(lib, sym);
    if (!fs[i]->code) fk_error("foreign import `%s` is missing from %s", fs[i]->name, so);
  }
}
