/* ffi.c -- foreign imports (ffi.md). ffi_glue writes one C wrapper per
   import, with funk's calling convention. Compiled programs include the
   wrappers; the interpreter compiles them with $CC into a shared library,
   cached by the hash of its source, and loads it. Both backends therefore
   run the same conversions. */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "funk.h"

Strs ffi_libs, ffi_cc_args;

/* The funk types that pass to C, and how each is held: 'i' as an Int,
   'f' as a Float, 'c' Char, 'b' Bool, 'v' (). The C types wrap to their
   width in their Num instances, so a value always fits. */
const FfiType ffi_types[] = {
    {"Int", "int64_t", 'i'},       {"Float", "double", 'f'},    {"Char", "char", 'c'},          {"Bool", "bool", 'b'},
    {"()", "void", 'v'},           {"CInt", "int", 'i'},        {"CUInt", "unsigned int", 'i'}, {"CLong", "long", 'i'},
    {"CULong", "unsigned long", 'i'}, {"CLLong", "long long", 'i'}, {"CSize", "size_t", 'i'},   {"CFloat", "float", 'f'},
};
const int ffi_ntypes = sizeof ffi_types / sizeof ffi_types[0];

static const FfiType *ffi_type(Type *t) {
  t = repr(t);
  if (t->k != TY_CON || t->n) return NULL;
  for (int i = 0; i < ffi_ntypes; i++)
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

static const char *unbox(Type *t, const char *v) {
  const FfiType *f = ffi_type(t);
  switch (f->kind) {
  case 'i': return xsprintf("(%s)FK_INT(%s)", f->ctype, v);
  case 'f': return xsprintf("(%s)FK_FLOATV(%s)", f->ctype, v);
  case 'c': return xsprintf("(char)FK_CHAR(%s)", v);
  default: return xsprintf("FK_ISTRUE(%s)", v);
  }
}

static const char *box(Type *t, const char *x) {
  switch (ffi_kind(t)) {
  case 'i': return xsprintf("FK_MKINT((int64_t)(%s))", x);
  case 'f': return xsprintf("fk_float((double)(%s))", x);
  case 'c': return xsprintf("FK_MKCHAR((unsigned char)(%s))", x);
  case 'b': return xsprintf("FK_BOOL((%s) != 0)", x);
  default: return xsprintf("((void)(%s), fk_unit)", x);
  }
}

char *ffi_glue(ForeignDecl **fs, int n, bool exported) {
  SB b = {0};
  sb_puts(&b, "\n/* ---- foreign imports ---- */\n");
  for (int i = 0; i < n; i++) {
    bool seen = false;
    for (int j = 0; j < i; j++) seen |= fs[j]->header == fs[i]->header;
    if (!seen) sb_printf(&b, "#include <%s>\n", fs[i]->header);
  }
  for (int i = 0; i < n; i++) {
    ForeignDecl *f = fs[i];
    SB call = {0}, proto = {0};
    Type *t = repr(globals.a[f->gidx].type);
    sb_printf(&call, "%s%s", f->cname, f->arity ? "(" : "");
    for (int k = 0; k < f->arity; k++, t = repr(t->args[1])) {
      sb_printf(&proto, "%s%s", k ? ", " : "", ffi_param_type(t->args[0]));
      if (ffi_kind(t->args[0]) == 'v') continue; /* f () */
      sb_printf(&call, "%s%s", k ? ", " : "", unbox(t->args[0], xsprintf("a[%d]", k)));
    }
    if (f->arity) sb_puts(&call, ")");
    /* The declared type must be the C one: a wrong width or arity would be
       converted silently. A function-like macro has no type to compare. */
    const char *ctype = f->arity ? xsprintf("%s (%s)", ffi_param_type(t), proto.s) : ffi_param_type(t);
    if (f->arity) sb_printf(&b, "#ifndef %s\n", f->cname);
    sb_printf(&b, "_Static_assert(__builtin_types_compatible_p(__typeof__(%s), %s), \"foreign import %s :: %s needs %s `%s` to have the C type %s; "
                  "see %s\");\n",
              f->cname, ctype, f->name, type_str(globals.a[f->gidx].type), f->arity ? "function" : "value", f->cname, ctype, f->header);
    if (f->arity) sb_puts(&b, "#endif\n");
    sb_printf(&b, "%sV fk_ffi_%d(V self, V *a) { (void)self; (void)a; return %s; } /* %s */\n", exported ? "" : "static ",
              f->gidx, box(t, call.s), f->name);
  }
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
      fk_error("foreign imports failed to compile with %s; its messages are above (source: %s)", cc, c);
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
