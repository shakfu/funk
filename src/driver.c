/* driver.c -- the library's api (libfunk.h): the prelude, files, the
   backends and REPL input, over the passes in funk.h. */
#define _DEFAULT_SOURCE
#include <unistd.h>

#include "funk.h"
#include "libfunk.h"

#ifndef FUNK_VERSION
#define FUNK_VERSION "dev" /* CMake defines it from the project version */
#endif

static Chunk prelude;
static bool prelude_installed; /* in the interpreter: build and check do not need it there */
static Strs cc_args;

static void report(const char *fmt, const char *arg) {
  fflush(stdout);
  fprintf(stderr, "funk: ");
  fprintf(stderr, fmt, arg);
  fputc('\n', stderr);
}

const char *funk_version(void) { return FUNK_VERSION; }

typedef struct { int (*fn)(void *); void *arg; int status; } Main;

static void main_task(void *arg) {
  Main *m = arg;
  m->status = m->fn(m->arg);
}

int funk_main(int (*fn)(void *), void *arg) {
  Main m = {fn, arg, 1};
  return fk_run(main_task, &m) ? 1 : m.status;
}

/* Parse with compile errors caught; NULL on error (already reported). */
static Module *parse_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = cerr_jmp;
  Module *volatile m = NULL;
  if (!setjmp(jb)) {
    cerr_jmp = &jb;
    m = parse_module(src, len);
  }
  cerr_jmp = saved;
  return m;
}

static Expr *parse_expr_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = cerr_jmp;
  Expr *volatile e = NULL;
  if (!setjmp(jb)) {
    cerr_jmp = &jb;
    e = parse_expr_text(src, len);
  }
  cerr_jmp = saved;
  return e;
}

static int is_decl_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = cerr_jmp;
  volatile int r = -1;
  if (!setjmp(jb)) {
    cerr_jmp = &jb;
    r = is_decl_text(src, len);
  }
  cerr_jmp = saved;
  return r;
}

bool funk_init(const char *const *args, int nargs) {
  for (int i = 0; i < nargs; i++) VPUSH(cc_args, args[i]);
  ffi_cc_args = cc_args;
  compiler_init();
  cur_file = "<prelude>";
  char *src = xsprintf("%s\n%s", fk_prelude_src, builtin_instances_src());
  Module *m = parse_caught(src, strlen(src));
  if (m && compile_chunk(m, &prelude)) return true;
  report("%s", "internal error: prelude failed to compile");
  return false;
}

static void install_prelude(void) {
  if (prelude_installed) return;
  interp_install(&prelude);
  prelude_installed = true;
}

static bool compile_file(const char *path, Chunk *out) {
  size_t len;
  char *src = read_file(path, &len);
  if (!src) {
    report("cannot read %s", path);
    return false;
  }
  cur_file = path;
  Module *m = parse_caught(src, len);
  return m && compile_chunk(m, out);
}

/* Compile path, which must define `main`. */
static bool compile_program(const char *path, Chunk *out) {
  int first = globals.n;
  if (!compile_file(path, out)) return false;
  if (global_lookup(intern("main")) >= first) return true;
  report("%s: no `main` defined", path);
  return false;
}

bool funk_check_file(const char *path) {
  Chunk c;
  return compile_file(path, &c);
}

/* Runtime errors in fn return here, after fk_error reports them. */
#define CATCH_RUNTIME(failed)    \
  jmp_buf rt;                    \
  fk_error_jmp = &rt;            \
  if (setjmp(rt)) {              \
    fk_error_jmp = NULL;         \
    failed;                      \
  }

bool funk_run_file(const char *path) {
  Chunk c;
  if (!compile_program(path, &c)) return false;
  CATCH_RUNTIME(return false);
  install_prelude();
  interp_install(&c);
  fk_error_jmp = NULL;
  fflush(stdout);
  return true;
}

char *funk_emit_c(const char *path) {
  Chunk c;
  return compile_program(path, &c) ? cgen_program((Chunk[]){prelude, c}, 2) : NULL;
}

static int run_cc(const char *csrc, const char *exe, const char *opt) {
  Argv argv = cc_argv();
  char *fixed[] = {xsprintf("-O%s", opt), "-o", (char *)exe, (char *)csrc, NULL};
  for (int i = 0; fixed[i]; i++) VPUSH(argv, fixed[i]);
  for (int i = 0; i < cc_args.n; i++) VPUSH(argv, (char *)cc_args.a[i]);
  for (int i = 0; i < ffi_libs.n; i++) VPUSH(argv, xsprintf("-l%s", ffi_libs.a[i])); /* before -lm, which they may use */
  static const char *const runtime_libs[] = {"-lm", "-lpthread"};
  for (int i = 0; i < 2; i++) {
    bool given = false; /* the linker warns about a library named twice */
    for (int j = 0; j < cc_args.n; j++) given |= strcmp(cc_args.a[j], runtime_libs[i]) == 0;
    for (int j = 0; j < ffi_libs.n; j++) given |= strcmp(ffi_libs.a[j], runtime_libs[i] + 2) == 0;
    if (!given) VPUSH(argv, (char *)runtime_libs[i]);
  }
  int status = spawn_wait(&argv);
  if (status < 0) report("cannot run %s", argv.a[0]);
  return status;
}

int funk_build(const char *path, const char *exe, const char *opt) {
  char *c = funk_emit_c(path);
  if (!c) return -1;
  const char *dir = getenv("TMPDIR");
  char *tmp = xsprintf("%s/funkXXXXXX.c", dir && *dir ? dir : "/tmp");
  int fd = mkstemps(tmp, 2);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if (!f) {
    report("cannot create %s", tmp);
    return -1;
  }
  fputs(c, f);
  fclose(f);
  int rc = run_cc(tmp, exe, opt);
  unlink(tmp);
  return rc;
}

bool funk_load_file(const char *path) {
  Chunk c;
  int mark = undo_mark(); /* fixities installed by the parser */
  const char *file = cur_file;
  bool ok = false;
  CATCH_RUNTIME(cur_file = file; return false);
  install_prelude();
  if (compile_file(path, &c)) {
    interp_install(&c);
    ok = true;
  } else {
    undo_to(mark);
  }
  fk_error_jmp = NULL;
  cur_file = file;
  return ok;
}

/* A value shown with its Show instance. Without one, shown structurally;
   a function shows as `<function> :: type`. */
static char *show_result(Bind *it) {
  Bind *b = NEW(Bind);
  b->name = intern("$shown");
  b->line = it->line;
  b->col = it->col;
  Expr *f = NEW(Expr), *x = NEW(Expr);
  *f = (Expr){.k = E_VAR, .line = b->line, .col = b->col, .name = intern("show")};
  *x = (Expr){.k = E_VAR, .line = b->line, .col = b->col, .name = it->name};
  b->e = NEW(Expr);
  *b->e = (Expr){.k = E_APP, .line = b->line, .col = b->col, .fn = f, .n = 1, .args = xmalloc(sizeof(Expr *))};
  b->e->args[0] = x;
  Module m = {.binds = &b, .nb = 1};
  Chunk c;
  cerr_quiet = true;
  bool ok = compile_chunk(&m, &c);
  cerr_quiet = false;
  if (ok) {
    interp_install(&c);
    return fk_cstring(interp_global(b->gidx), NULL);
  }
  V v = interp_global(it->gidx);
  if (FK_ISPTR(v) && FK_HDR(v)->kind != K_CON && FK_HDR(v)->kind != K_FLOAT)
    return xsprintf("<function> :: %s", type_str(globals.a[it->gidx].type));
  return fk_show_cstr(v);
}

bool funk_eval(const char *src, const char *name, char **shown) {
  size_t len = strlen(src);
  const char *file = cur_file;
  *shown = NULL;
  cur_file = name;
  CATCH_RUNTIME(cur_file = file; return false);
  install_prelude();
  bool ok = false;
  int decl = is_decl_caught(src, len);
  Chunk c;
  if (decl > 0) {
    int mark = undo_mark(); /* fixities installed by the parser */
    Module *m = parse_caught(src, len);
    if (m && compile_chunk(m, &c)) {
      interp_install(&c);
      ok = true;
    } else {
      undo_to(mark);
    }
  } else if (decl == 0) {
    Expr *e = parse_expr_caught(src, len);
    Bind *b = NEW(Bind); /* bound as $it so `it` in e means the previous result */
    b->name = intern("$it");
    b->e = e;
    Module m = {.binds = &b, .nb = 1};
    if (e) {
      b->line = e->line;
      b->col = e->col;
    }
    if (e && compile_chunk(&m, &c)) {
      interp_install(&c);
      global_alias(intern("it"), b->gidx);
      if (interp_global(b->gidx) != fk_unit) *shown = show_result(b);
      ok = true;
    }
  }
  fk_error_jmp = NULL;
  cur_file = file;
  return ok;
}

char *funk_type_of(const char *src, const char *name) {
  const char *file = cur_file;
  cur_file = name;
  Expr *e = parse_expr_caught(src, strlen(src));
  const char *t = e ? infer_type_text(e) : NULL;
  cur_file = file;
  return t ? xsprintf("%s", t) : NULL;
}
