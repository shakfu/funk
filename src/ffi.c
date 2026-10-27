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
Foreigns ffi_exports;

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
  static const FfiType field = {"Field", "int64_t", 'i', 8};
  if (t->n == 2 && !strcmp(t->name, "Field")) return &field; /* an offset */
  if (t->n == 1 && (!strcmp(t->name, "Ptr") || !strcmp(t->name, "FunPtr") || !strcmp(t->name, "StablePtr") || !strcmp(t->name, "Array") ||
                    !strcmp(t->name, "Ring")))
    return &ptr; /* an Array or Ring passes as its header's address */
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
  case 'p': return xsprintf("fk_ptr((void *)(%s))", x); /* the C type may be const */
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
  SB params = {0}, fields = {0}, fill = {0}, frees = {0}, args = {0}, nulls = {0};
  bool q = f->cb == 'q';
  int n = 0, id = f->gidx;
  sb_puts(&params, "void *ctx");
  for (r = repr(t->args[1]); r->k == TY_CON && !strcmp(r->name, "->"); r = repr(r->args[1]), n++) {
    Type *a = r->args[0];
    int k = ffi_kind(a);
    const char *x = xsprintf("x%d", n), *v = q ? xsprintf("d->x%d", n) : x;
    if (k == 'L') sb_printf(&params, ", const char *%s, size_t %sn", x, x);
    else sb_printf(&params, ", %s %s", ffi_param_type(a), x);
    if (k == 'L') sb_printf(&args, "%sfk_str(%s ? %s : \"\", %sn)", n ? ", " : "", v, v, v);
    else if (k == 's') sb_printf(&args, "%sfk_cstr_result(%s, \"%s\")", n ? ", " : "", v, f->name);
    else sb_printf(&args, "%s%s", n ? ", " : "", box(a, v));
    if (!q) continue;
    if (k == 'L' || k == 's') { /* bytes go into the slot, or are malloc'd if they do not fit */
      const char *len = k == 'L' ? xsprintf("%sn", x) : xsprintf("%s ? strlen(%s) + 1 : 0", x, x);
      sb_printf(&fields, k == 'L' ? " char *%s; size_t %sn;" : " char *%s;", x, x);
      sb_printf(&fill, "  d->%s = fk_event_copy(e, &used, %s, %s);\n  if (%s && !d->%s) e->run = NULL;\n", x, x, len, x, x);
      if (k == 'L') sb_printf(&fill, "  d->%sn = %sn;\n", x, x);
      sb_printf(&frees, "  fk_event_uncopy(e, d->%s);\n", x);
      if (k == 's') sb_printf(&nulls, "%s!d->%s", nulls.n ? " || " : "", x);
    } else {
      sb_printf(&fields, " %s %s;", ffi_param_type(a), x);
      sb_printf(&fill, "  d->%s = %s;\n", x, x);
    }
  }
  if (!n) sb_puts(&args, "fk_unit");
  int na = n ? n : 1;
  const char *rt = ffi_param_type(r);
  if (q) { /* the arguments are a struct at the start of the slot's data */
    sb_printf(b, "struct fk_ev_%d {%s };\n_Static_assert(sizeof(struct fk_ev_%d) <= FK_EVENT_DATA, \"%s: too many arguments for an event\");\n", id,
              n ? fields.s : " char unused;", id, f->name);
    /* a NULL String is an error, raised once the slot is free: the ring must move on */
    const char *null_check = nulls.n ? xsprintf("  if (%s) {\n%s    fk_event_release(e);\n    fk_error(\"queued callback `%s`: C passed NULL for a String\");\n  }\n",
                                                nulls.s, frees.s, f->name) : "";
    sb_printf(b, "static void fk_evrun_%d(FkEvent *e) {\n  struct fk_ev_%d *d = (struct fk_ev_%d *)e->data;\n%s  V a[%d] = {%s};\n%s"
                 "  void *ctx = e->ctx;\n  fk_event_release(e);\n  fk_apply(fk_stable_deref(ctx), %d, a);\n}\n",
              id, id, id, null_check, na, args.s, frees.s, na);
    sb_printf(b, "static void fk_cb_%d(%s) { /* %s */\n  FkEvent *e = fk_event_claim();\n  if (!e) return; /* the ring is full: dropped, and counted */\n"
                 "  struct fk_ev_%d *d = (struct fk_ev_%d *)e->data;\n  size_t used = sizeof *d;\n  (void)used;\n  e->run = fk_evrun_%d;\n  e->ctx = ctx;\n%s"
                 "  fk_event_publish(e);\n}\n",
              id, params.s, f->name, id, id, id, fill.s);
  } else {
    int rk = ffi_kind(r);
    const char *ret = rk == 'v' ? "return;" : "return 0;"; /* C gets 0 while a failure is pending */
    sb_printf(b, "static %s fk_cb_%d(%s) { /* %s */\n  if (!fk_on_funk_thread()) fk_callback_thread(\"%s\");\n  if (fk_cb_failed) %s\n"
                 "  jmp_buf jb, *saved = fk_error_jmp;\n  if (setjmp(jb)) {\n    fk_error_jmp = saved;\n    fk_cb_failed = 1;\n    fk_callback_depth--;\n    %s\n  }\n"
                 "  fk_error_jmp = &jb;\n  fk_callback_depth++;\n  V a[%d] = {%s};\n  V r = fk_apply(fk_stable_deref(ctx), %d, a);\n  fk_callback_depth--;\n  fk_error_jmp = saved;\n",
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
  Strs hs = {0}; /* each header once: some have no include guard */
  for (int i = 0; i < n; i++)
    for (int k = -1; k < fs[i]->xheaders.n; k++) {
      const char *h = k < 0 ? fs[i]->header : fs[i]->xheaders.a[k];
      bool seen = !h;
      for (int j = 0; j < hs.n && !seen; j++) seen = hs.a[j] == h;
      if (seen) continue;
      VPUSH(hs, h);
      sb_printf(&b, "#include <%s>\n", h);
    }
  for (int i = 0; i < n; i++) { /* foreign structs: definitions, and checks of header structs; once each */
    bool seen = false;
    for (int j = 0; j < i; j++) seen |= fs[j]->cdef == fs[i]->cdef;
    if (fs[i]->cdef && !seen) sb_puts(&b, fs[i]->cdef);
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
    case 'p': sb_printf(&body, "  const void *r = %s; %s\n  fk_cb_check();\n  return fk_ptr((void *)r);\n", call.s, what); break;
    default: sb_printf(&body, "  %s r = %s; %s\n  fk_cb_check();\n  return %s;\n", ffi_param_type(t), call.s, what, box(t, "r"));
    }
    sb_printf(&b, "%sV fk_ffi_%d(V self, V *a) {\n  (void)self; (void)a;\n%s%s}\n", exported ? "" : "static ", f->gidx,
              f->arity ? "  fk_ffi_enter();\n" : "", body.s);
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

/* A cached library is stale if a header its glue read is newer: the
   compiler lists them with -MD in so.d. */
static bool headers_newer(const char *so) {
  struct stat ss, hs;
  if (stat(so, &ss) != 0) return true;
  size_t len;
  char *d = read_file(xsprintf("%s.d", so), &len);
  if (!d) return true;
  bool newer = false;
  for (char *w = strtok(d, " \t\n\\"); w && !newer; w = strtok(NULL, " \t\n\\"))
    if (w[strlen(w) - 1] != ':') newer = stat(w, &hs) != 0 || hs.st_mtime > ss.st_mtime;
  free(d);
  return newer;
}

/* The libraries named, as found in the -L directories: their size and
   time, so a rebuilt library rebuilds the glue that links it. */
static uint64_t hash_libraries(uint64_t h) {
  static const char *const ext[] = {".a", ".dylib", ".so"};
  for (int i = 0; i < ffi_cc_args.n; i++) {
    if (strncmp(ffi_cc_args.a[i], "-L", 2) != 0) continue;
    for (int j = 0; j < ffi_libs.n + ffi_cc_args.n; j++) {
      const char *name = j < ffi_libs.n ? ffi_libs.a[j] : !strncmp(ffi_cc_args.a[j - ffi_libs.n], "-l", 2) ? ffi_cc_args.a[j - ffi_libs.n] + 2 : NULL;
      for (size_t k = 0; name && k < sizeof ext / sizeof ext[0]; k++) {
        struct stat st;
        if (stat(xsprintf("%s/lib%s%s", ffi_cc_args.a[i] + 2, name, ext[k]), &st) == 0)
          h = hash(h, xsprintf("%lld %lld", (long long)st.st_size, (long long)st.st_mtime));
      }
    }
  }
  return h;
}

void ffi_load(ForeignDecl **fs, int n) {
  if (!n) return;
  Argv argv = cc_argv();
  char *src = xsprintf("%s%s", fk_rth_src, ffi_glue(fs, n, true));
  uint64_t h = hash(14695981039346656037ULL, src); /* the compiler and libraries too, below */
  for (int i = 0; i < argv.n; i++) h = hash(h, argv.a[i]);
  for (int i = 0; i < ffi_libs.n; i++) h = hash(h, ffi_libs.a[i]);
  for (int i = 0; i < ffi_cc_args.n; i++) h = hash(h, ffi_cc_args.a[i]);
  h = hash_libraries(h);
  const char *dir = cache_dir();
  char *so = xsprintf("%s/%016llx.so", dir, (unsigned long long)h);
  if (access(so, R_OK) == 0 && !headers_newer(so)) {
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
                           "-o", tmp, c, "-MD", "-MF", xsprintf("%s.d", so)};
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
