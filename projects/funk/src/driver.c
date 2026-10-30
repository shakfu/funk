/* driver.c -- the library's api (libfunk.h): the prelude, files, the
   backends and REPL input, over the passes in funk.h. */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <unistd.h>

#include "funk.h"
#include "libfunk.h"

#ifndef FUNK_VERSION
#define FUNK_VERSION "dev" /* CMake defines it from the project version */
#endif

/* Imported modules (modules.md): each file once, keyed by realpath, its
   chunk compiled before the files that import it. */
typedef struct { const char *real, *file, *prefix; bool done; Exports *ex; } Mod; /* file: as found, for messages */

/* driver.c's state, one per session (embedding.md, phase 2) */
typedef struct {
  Chunk prelude;
  bool prelude_installed; /* in the interpreter: build and check do not need it there */
  Strs cc_args;
  VEC(Mod) mods;
  VEC(Chunk) mod_chunks;
  int mods_installed; /* in the interpreter */
  Strs mod_dirs; /* -M DIR */
  ImportInfo *last_imps; /* the program's imports, resolved by the last prepare */
  int nlast_imps;
  VEC(ImportInfo) repl_imps; /* in scope at the prompt: `import` inputs and :l files' imports */
  Exports prepare_none;
  int funk_call_checked, funk_call_checked_n; /* type_str allocates: check a global once */
} DriverState;
static const DriverState driver_init = {.funk_call_checked = -1};
#define ST ((DriverState *)fk_here()->cc[CC_DRIVER])

void *driver_state_new(void) {
  DriverState *s = malloc(sizeof *s);
  if (!s) fk_fatal("out of memory");
  *s = driver_init;
  return s;
}


/* The REPL's imports as Import lines, for the parser's qualified tokens. */
static Import *repl_lines(void) {
  Import *ims = xmalloc((size_t)ST->repl_imps.n * sizeof *ims + 1);
  for (int i = 0; i < ST->repl_imps.n; i++) ims[i] = *ST->repl_imps.a[i].im;
  return ims;
}

/* The last program's imports into the REPL's scope; a qualifier bound
   before is rebound. */
static void adopt_imports(void) {
  for (int i = 0; i < ST->nlast_imps; i++) {
    int k = 0;
    while (k < ST->repl_imps.n && ST->repl_imps.a[k].im->qual != ST->last_imps[i].im->qual) k++;
    if (k == ST->repl_imps.n) VPUSH(ST->repl_imps, ST->last_imps[i]);
    else ST->repl_imps.a[k] = ST->last_imps[i];
  }
}

static void report(const char *fmt, const char *arg) { fk_message("funk: ", fmt, arg); }

const char *funk_version(void) { return FUNK_VERSION; }

/* A session (libfunk.h): its thread runs funk_run's jobs, one at a time,
   until funk_free (embedding.md, phase 2). */
struct funk {
  FkRt *rt;
  pthread_t th;
  pthread_mutex_t mu, run; /* run: held by a funk_run from another thread for its job */
  pthread_cond_t cv;
  int (*fn)(funk *, void *); /* the job; NULL when idle */
  void *arg;
  int status;
  bool ready, ok, done, stop;
  const char *const *cc_args; /* funk_config's, for the session's thread */
  int ncc_args;
};

/* The api's functions but funk_run, funk_free, funk_interrupt, funk_output
   and funk_on_fatal run on the session's thread. */
static bool on_thread(funk *f, const char *fn) {
  if (fk_current() == f->rt) return true;
  fk_message_rt(f->rt, "funk: ", "%s: not on its session's thread; call it inside funk_run", fn);
  return false;
}
#define ON_THREAD(f, fail) \
  if (!on_thread(f, __func__)) return fail

static bool init(void);

static const CompilerShared shared_init = {.cur_file = "<input>", .num_class = -1};

void *shared_state_new(void) {
  CompilerShared *s = malloc(sizeof *s);
  if (!s) fk_fatal("out of memory");
  *s = shared_init;
  return s;
}

/* The compiler's state for this session, on its thread. */
static void states_new(void) {
  static void *(*const make[CC_N])(void) = {
      [CC_SHARED] = shared_state_new, [CC_UTIL] = util_state_new,   [CC_PARSE] = parse_state_new, [CC_RESOLVE] = resolve_state_new,
      [CC_CHECK] = check_state_new,   [CC_ELAB] = elab_state_new,   [CC_FUSE] = fuse_state_new,   [CC_CGEN] = cgen_state_new,
      [CC_INTERP] = interp_state_new, [CC_FFI] = ffi_state_new,     [CC_DRIVER] = driver_state_new};
  for (int i = 0; i < CC_N; i++) fk_here()->cc[i] = make[i]();
}

static void session_main(void *p) {
  funk *f = p;
  states_new();
  for (int i = 0; i < f->ncc_args; i++) VPUSH(ST->cc_args, f->cc_args[i]);
  bool ok = init();
  pthread_mutex_lock(&f->mu);
  f->ok = ok;
  f->ready = true;
  pthread_cond_broadcast(&f->cv);
  while (ok) {
    while (!f->fn && !f->stop) pthread_cond_wait(&f->cv, &f->mu);
    if (f->stop) break;
    pthread_mutex_unlock(&f->mu);
    int status = f->fn(f, f->arg);
    pthread_mutex_lock(&f->mu);
    f->status = status;
    f->fn = NULL;
    f->done = true;
    pthread_cond_broadcast(&f->cv);
  }
  pthread_mutex_unlock(&f->mu);
  fflush(stdout);
}

static void session_free(funk *f) {
  pthread_mutex_destroy(&f->mu);
  pthread_mutex_destroy(&f->run);
  pthread_cond_destroy(&f->cv);
  free(f);
}

funk *funk_new(const funk_config *c) {
  funk *f = calloc(1, sizeof *f);
  if (!f) return NULL;
  if (c) {
    f->cc_args = c->cc_args;
    f->ncc_args = c->ncc_args;
  }
  pthread_mutex_init(&f->mu, NULL);
  pthread_mutex_init(&f->run, NULL);
  pthread_cond_init(&f->cv, NULL);
  if (!(f->rt = fk_start(session_main, f, &f->th, c ? c->heap : 0, c ? c->threshold : 0, c ? c->stack : 0))) {
    session_free(f);
    return NULL;
  }
  pthread_mutex_lock(&f->mu);
  while (!f->ready) pthread_cond_wait(&f->cv, &f->mu);
  pthread_mutex_unlock(&f->mu);
  if (f->ok) return f;
  fk_stop(f->rt, f->th); /* the thread has left its loop */
  session_free(f);
  return NULL;
}

int funk_run(funk *f, int (*fn)(funk *, void *), void *arg) {
  if (fk_current() == f->rt) return fn(f, arg); /* another session's thread queues, as any other thread */
  pthread_mutex_lock(&f->run);
  pthread_mutex_lock(&f->mu);
  f->fn = fn;
  f->arg = arg;
  f->done = false;
  pthread_cond_broadcast(&f->cv);
  while (!f->done) pthread_cond_wait(&f->cv, &f->mu);
  int status = f->status;
  pthread_mutex_unlock(&f->mu);
  pthread_mutex_unlock(&f->run);
  return status;
}

void funk_free(funk *f) {
  if (!f) return;
  if (fk_current() == f->rt) {
    report("%s", "funk_free: on its session's thread; call it from another");
    return;
  }
  pthread_mutex_lock(&f->mu);
  f->stop = true;
  pthread_cond_broadcast(&f->cv);
  pthread_mutex_unlock(&f->mu);
  fk_stop(f->rt, f->th);
  session_free(f);
}

/* Parse with compile errors caught; NULL on error (already reported). */
static Module *parse_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = CS->cerr_jmp;
  Module *volatile m = NULL;
  if (!setjmp(jb)) {
    CS->cerr_jmp = &jb;
    m = parse_module(src, len);
  }
  CS->cerr_jmp = saved;
  return m;
}

static Module *begin_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = CS->cerr_jmp;
  Module *volatile m = NULL;
  if (!setjmp(jb)) {
    CS->cerr_jmp = &jb;
    m = parse_begin(src, len);
  }
  CS->cerr_jmp = saved;
  return m;
}

static Expr *parse_expr_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = CS->cerr_jmp;
  Expr *volatile e = NULL;
  if (!setjmp(jb)) {
    CS->cerr_jmp = &jb;
    e = parse_expr_text(src, len, repl_lines(), ST->repl_imps.n);
    Bind *b = NEW(Bind); /* qualified through a module holding e */
    *b = (Bind){.name = intern("$q"), .e = e};
    if (ST->repl_imps.n) qualify_module(&(Module){.binds = &b, .nb = 1}, NULL, ST->repl_imps.a, ST->repl_imps.n);
    e = b->e;
  }
  CS->cerr_jmp = saved;
  return e;
}

static int is_decl_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = CS->cerr_jmp;
  volatile int r = -1;
  if (!setjmp(jb)) {
    CS->cerr_jmp = &jb;
    r = is_decl_text(src, len);
  }
  CS->cerr_jmp = saved;
  return r;
}

/* The prelude, compiled on the session's thread. */
static bool init(void) {
  CS->ffi_cc_args = ST->cc_args;
  compiler_init();
  CS->cur_file = "<prelude>";
  char *src = xsprintf("%s\n%s", fk_prelude_src, builtin_instances_src());
  Module *m = parse_caught(src, strlen(src));
  if (m && compile_chunk(m, &ST->prelude)) return true;
  report("%s", "internal error: prelude failed to compile");
  return false;
}

static void install_prelude(void) {
  if (ST->prelude_installed) return;
  interp_install(&ST->prelude);
  ST->prelude_installed = true;
}

void funk_module_dir(funk *f, const char *dir) {
  ON_THREAD(f, );
  VPUSH(ST->mod_dirs, dir);
}

bool funk_provide(funk *f, const char *name, void *addr) {
  ON_THREAD(f, false);
  bool ident = name && (isalpha((unsigned char)*name) || *name == '_');
  for (const char *c = name; ident && *c; c++) ident = isalnum((unsigned char)*c) || *c == '_';
  if (!ident || !addr) return false;
  ffi_provide(name, addr);
  return true;
}

void funk_glue(funk *f, const funk_glue_entry *glue, int n) {
  ON_THREAD(f, );
  for (int i = 0; i < n; i++) ffi_link(glue[i].key, (FkCode)glue[i].code);
}

static Exports *prepare(Module *m, const char *file, const char *prefix);

/* The file for module path a.b, looked for as a/b.fk beside from, then
   in each -M directory; its realpath, or NULL. */
static const char *find_module(const char *path, const char *from, const char **found, SB *searched) {
  char *rel = xsprintf("%s.fk", path);
  for (size_t i = 0, n = strlen(rel) - 3; i < n; i++)
    if (rel[i] == '.') rel[i] = '/';
  const char *slash = strrchr(from, '/');
  Strs dirs = {0};
  VPUSH(dirs, slash ? xsprintf("%.*s", (int)(slash - from), from) : ".");
  for (int i = 0; i < ST->mod_dirs.n; i++) VPUSH(dirs, ST->mod_dirs.a[i]);
  for (int i = 0; i < dirs.n; i++) {
    char *cand = xsprintf("%s/%s", dirs.a[i], rel);
    sb_printf(searched, "%s%s", i ? ", " : "", cand);
    char *real = access(cand, R_OK) == 0 ? realpath(cand, NULL) : NULL;
    if (real) {
      *found = cand;
      free(dirs.a);
      return intern(real);
    }
  }
  free(dirs.a);
  return NULL;
}

/* Compile the module im names, once; its entry in mods, or -1 after an
   error inside it. Other errors unwind through cerror to prepare_caught. */
static int import_module(Import *im, const char *from) {
  const char *found;
  SB searched = {0};
  const char *real = find_module(im->path, from, &found, &searched);
  if (!real) cerror(im->line, im->col, "module `%s` not found; looked for %s", im->path, searched.s);
  for (int i = 0; i < ST->mods.n; i++) {
    if (ST->mods.a[i].real != real) continue;
    if (ST->mods.a[i].done) return i;
    SB chain = {0}; /* the modules still loading, from this one, are the cycle */
    for (int j = i; j < ST->mods.n; j++) sb_printf(&chain, "%s -> ", ST->mods.a[j].prefix);
    cerror(im->line, im->col, "import cycle: %s%s", chain.s, ST->mods.a[i].prefix);
  }
  const char *dot = strrchr(im->path, '.'), *prefix = intern(dot ? dot + 1 : im->path);
  for (int i = 0; i < ST->mods.n; i++)
    if (ST->mods.a[i].prefix == prefix)
      cerror(im->line, im->col, "two modules are named `%s`: %s, and %s imported earlier", prefix, found, ST->mods.a[i].file);
  VPUSH(ST->mods, ((Mod){real, found, prefix, false, NULL}));
  int me = ST->mods.n - 1;
  size_t len;
  char *src = read_file(found, &len);
  if (!src) cerror(im->line, im->col, "cannot read %s", found);
  CS->cur_file = found;
  Module *m = parse_begin(src, len);
  if (m->name) {
    const char *hd = strrchr(m->name, '.');
    if (strcmp(hd ? hd + 1 : m->name, prefix)) cerror(m->line, m->col, "the module is named `%s`, but it is imported as `%s`", m->name, im->path);
  }
  Chunk c;
  Exports *ex = prepare(m, found, prefix);
  if (!ex) return -1;
  CS->cur_file = found;
  const char *cm = CS->cur_module;
  CS->cur_module = prefix;
  bool ok = compile_chunk(m, &c);
  CS->cur_module = cm;
  if (!ok) return -1;
  VPUSH(ST->mod_chunks, c);
  ST->mods.a[me].done = true;
  ST->mods.a[me].ex = ex;
  CS->cur_file = from;
  return me;
}

/* Compile m's imports, parse the rest of m, then qualify it: with prefix
   for a module, NULL for the program. What m exports, or NULL after an
   error. */
static Exports *prepare(Module *m, const char *file, const char *prefix) {
  if (!m->nimports && !prefix) {
    parse_finish(m);
    ST->nlast_imps = 0;
    return &ST->prepare_none;
  }
  ImportInfo *imps = xmalloc((size_t)m->nimports * sizeof *imps + 1);
  for (int i = 0; i < m->nimports; i++) {
    CS->cur_file = file;
    Import *im = &m->imports[i];
    if (im->base) { /* the prelude stands in for it: names checked, ST->prepare_none added (modules.md) */
      static const char *const base[] = {"Prelude", "Data.List", "Data.Char", "Data.Maybe", "Data.Either", "Data.Bits",
                                         "Data.Function", "Data.Tuple", "Data.Ord", "Control.Monad", "System.IO", NULL};
      int b = 0;
      while (base[b] && strcmp(base[b], im->path)) b++;
      if (!base[b])
        cerror(im->line, im->col, "funk has no module `%s`; a capitalized import names a Haskell base module that the prelude stands in for: "
               "Prelude, Data.List, Data.Char, Data.Maybe, Data.Either, Data.Bits, Data.Function, Data.Tuple, Data.Ord, Control.Monad, System.IO",
               im->path);
      imps[i] = (ImportInfo){im, NULL, NULL};
      continue;
    }
    int k = import_module(im, file);
    if (k < 0) return NULL;
    imps[i] = (ImportInfo){&m->imports[i], ST->mods.a[k].prefix, ST->mods.a[k].ex};
  }
  CS->cur_file = file;
  parse_finish(m);
  if (!prefix) {
    ST->last_imps = imps;
    ST->nlast_imps = m->nimports;
  }
  return qualify_module(m, prefix, imps, m->nimports);
}

static bool prepare_caught(Module *m, const char *file) {
  jmp_buf jb, *saved = CS->cerr_jmp;
  const char *cf = CS->cur_file;
  int n0 = ST->mods.n;
  volatile bool ok = false;
  if (!setjmp(jb)) {
    CS->cerr_jmp = &jb;
    ok = prepare(m, file, NULL) != NULL;
  }
  CS->cerr_jmp = saved;
  CS->cur_file = cf;
  if (!ok) { /* forget the modules left half-loaded; compiled ones stay */
    int k = n0;
    for (int i = n0; i < ST->mods.n; i++)
      if (ST->mods.a[i].done) ST->mods.a[k++] = ST->mods.a[i];
    ST->mods.n = k;
  }
  return ok;
}

/* The rest of a REPL declaration, qualified with the REPL's imports. */
static bool finish_caught(Module *m) {
  jmp_buf jb, *saved = CS->cerr_jmp;
  volatile bool ok = false;
  if (!setjmp(jb)) {
    CS->cerr_jmp = &jb;
    parse_finish(m);
    if (ST->repl_imps.n) qualify_module(m, NULL, ST->repl_imps.a, ST->repl_imps.n);
    ok = true;
  }
  CS->cerr_jmp = saved;
  return ok;
}

static void install_modules(void) {
  for (; ST->mods_installed < ST->mod_chunks.n; ST->mods_installed++) interp_install(&ST->mod_chunks.a[ST->mods_installed]);
}

static bool compile_file(const char *path, Chunk *out) {
  size_t len;
  char *src = read_file(path, &len);
  if (!src) {
    report("cannot read %s", path);
    return false;
  }
  CS->cur_file = path;
  Module *m = begin_caught(src, len);
  return m && prepare_caught(m, path) && compile_chunk(m, out);
}

/* Compile path, which must define `main`. */
static bool compile_program(const char *path, Chunk *out) {
  int first = CS->globals.n;
  if (!compile_file(path, out)) return false;
  if (global_lookup(intern("main")) >= first) return true;
  report("%s: no `main` defined", path);
  return false;
}

bool funk_check_file(funk *f, const char *path) {
  ON_THREAD(f, false);
  Chunk c;
  return compile_file(path, &c);
}

/* Runtime errors in fn return here, after fk_error reports them. */
#define CATCH_RUNTIME(failed)    \
  jmp_buf rt;                    \
  fk_error_jmp = &rt;            \
  if (setjmp(rt)) {              \
    fk_error_jmp = NULL;         \
    fk_recover();                \
    failed;                      \
  }

bool funk_run_file(funk *f, const char *path) {
  ON_THREAD(f, false);
  atomic_store(&fk_interrupt, 0);
  Chunk c;
  if (!compile_program(path, &c)) return false;
  CATCH_RUNTIME(return false);
  install_prelude();
  install_modules();
  interp_install(&c);
  fk_error_jmp = NULL;
  fflush(stdout);
  return true;
}

char *funk_emit_c(funk *f, const char *path) {
  ON_THREAD(f, NULL);
  Chunk c;
  if (!compile_program(path, &c)) return NULL;
  int n = ST->mod_chunks.n + 2;
  Chunk *all = xmalloc((size_t)n * sizeof *all);
  all[0] = ST->prelude;
  copy_n(all + 1, ST->mod_chunks.a, (size_t)ST->mod_chunks.n * sizeof *all);
  all[n - 1] = c;
  return cgen_program(all, n);
}

char *funk_emit_kernels(funk *f, const char *path) {
  ON_THREAD(f, NULL);
  Chunk c;
  if (!compile_file(path, &c)) return NULL;
  int n = ST->mod_chunks.n + 2;
  Chunk *all = xmalloc((size_t)n * sizeof *all);
  all[0] = ST->prelude;
  copy_n(all + 1, ST->mod_chunks.a, (size_t)ST->mod_chunks.n * sizeof *all);
  all[n - 1] = c;
  const char *err = NULL;
  char *src = cgen_kernels(all, n, &err);
  if (!src) report("%s", xsprintf("%s: %s", path, err));
  return src;
}

char *funk_emit_glue(funk *f, const char *path) {
  ON_THREAD(f, NULL);
  Chunk c;
  if (!compile_file(path, &c)) return NULL;
  VEC(ForeignDecl *) fs = {0};
  for (int i = 0; i < CS->globals.n; i++)
    if (CS->globals.a[i].ffi) VPUSH(fs, CS->globals.a[i].ffi);
  return ffi_linked_glue(fs.a, fs.n);
}

static int run_cc(const char *csrc, const char *exe, const char *opt) {
  Argv argv = cc_argv();
  char *fixed[] = {xsprintf("-O%s", opt), "-o", (char *)exe, (char *)csrc, NULL};
  for (int i = 0; fixed[i]; i++) VPUSH(argv, fixed[i]);
  for (int i = 0; i < ST->cc_args.n; i++) VPUSH(argv, (char *)ST->cc_args.a[i]);
  for (int i = 0; i < CS->ffi_libs.n; i++) VPUSH(argv, xsprintf("-l%s", CS->ffi_libs.a[i])); /* before -lm, which they may use */
  static const char *const runtime_libs[] = {"-lm", "-lpthread"};
  for (int i = 0; i < 2; i++) {
    bool given = false; /* the linker warns about a library named twice */
    for (int j = 0; j < ST->cc_args.n; j++) given |= strcmp(ST->cc_args.a[j], runtime_libs[i]) == 0;
    for (int j = 0; j < CS->ffi_libs.n; j++) given |= strcmp(CS->ffi_libs.a[j], runtime_libs[i] + 2) == 0;
    if (!given) VPUSH(argv, (char *)runtime_libs[i]);
  }
  int status = spawn_wait(&argv);
  if (status < 0) report("cannot run %s", argv.a[0]);
  return status;
}

int funk_build(funk *f, const char *path, const char *exe, const char *opt) {
  char *c = funk_emit_c(f, path);
  if (!c) return -1;
  const char *dir = getenv("TMPDIR");
  char *tmp = xsprintf("%s/funkXXXXXX.c", dir && *dir ? dir : "/tmp");
  int fd = mkstemps(tmp, 2);
  FILE *out = fd < 0 ? NULL : fdopen(fd, "w");
  if (!out) {
    report("cannot create %s", tmp);
    return -1;
  }
  fputs(c, out);
  fclose(out);
  int rc = run_cc(tmp, exe, opt);
  unlink(tmp);
  return rc;
}

bool funk_load_file(funk *f, const char *path) {
  ON_THREAD(f, false);
  atomic_store(&fk_interrupt, 0);
  Chunk c;
  int mark = undo_mark(); /* fixities installed by the parser */
  const char *file = CS->cur_file;
  bool ok = false;
  CATCH_RUNTIME(CS->cur_file = file; return false);
  install_prelude();
  if (compile_file(path, &c)) {
    install_modules();
    interp_install(&c);
    adopt_imports();
    ok = true;
  } else {
    undo_to(mark);
  }
  fk_error_jmp = NULL;
  CS->cur_file = file;
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
  CS->cerr_quiet = true;
  bool ok = compile_chunk(&m, &c);
  CS->cerr_quiet = false;
  if (ok) {
    interp_install(&c);
    return fk_cstring(interp_global(b->gidx), NULL);
  }
  V v = interp_global(it->gidx);
  if (FK_ISPTR(v) && FK_HDR(v)->kind != K_CON && FK_HDR(v)->kind != K_FLOAT)
    return xsprintf("<function> :: %s", type_str(CS->globals.a[it->gidx].type));
  return fk_show_cstr(v);
}

bool funk_eval(funk *f, const char *src, const char *name, char **shown) {
  *shown = NULL;
  ON_THREAD(f, false);
  atomic_store(&fk_interrupt, 0);
  size_t len = strlen(src);
  const char *file = CS->cur_file;
  *shown = NULL;
  CS->cur_file = name;
  CATCH_RUNTIME(CS->cur_file = file; return false);
  install_prelude();
  bool ok = false;
  int decl = is_decl_caught(src, len);
  Chunk c;
  const char *s = src + strspn(src, " \t");
  if (!strncmp(s, "import", 6) && (s[6] == ' ' || s[6] == '\t')) { /* modules.md, phase 3 */
    Module *m = begin_caught(src, len);
    if (m && prepare_caught(m, name)) {
      if (m->nb || m->nd || m->ncl || m->ni || m->nforeign) {
        report("%s", "an `import` input holds only imports");
      } else {
        install_modules();
        adopt_imports();
        ok = true;
      }
    }
  } else if (decl > 0) {
    int mark = undo_mark(); /* fixities installed by the parser */
    Module *m = begin_caught(src, len);
    if (m) { /* in the REPL's scope */
      m->imports = repl_lines();
      m->nimports = ST->repl_imps.n;
    }
    if (m && !finish_caught(m)) m = NULL;
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
  CS->cur_file = file;
  return ok;
}

char *funk_type_of(funk *f, const char *src, const char *name) {
  ON_THREAD(f, NULL);
  const char *file = CS->cur_file;
  CS->cur_file = name;
  Expr *e = parse_expr_caught(src, strlen(src));
  const char *t = e ? infer_type_text(e) : NULL;
  CS->cur_file = file;
  return t ? xsprintf("%s", t) : NULL;
}

/* The type funk_call accepts for n arguments: every arrow `->` or `->>`. */
static bool int_function(Type *t, int n) {
  SB want = {0};
  sb_puts(&want, "");
  for (int i = 0; i < n; i++) sb_puts(&want, "Int -> ");
  sb_puts(&want, "Int");
  SB got = {0};
  sb_puts(&got, "");
  for (const char *s = type_str(t); *s; s++) {
    sb_printf(&got, "%c", *s);
    if (s[0] == '-' && s[1] == '>' && s[2] == '>') s++; /* `->>` as `->` */
  }
  return strcmp(want.s, got.s) == 0;
}

void funk_on_fatal(funk *f, void (*fn)(void *ctx, const char *msg), void *ctx) {
  (void)f;
  fk_fatal_hook = fn;
  fk_fatal_ctx = ctx;
}

void funk_interrupt(funk *f) { fk_rt_interrupt(f->rt); }

void funk_output(funk *f, void (*out)(void *ctx, int stream, const char *s, size_t n), void *ctx) { fk_rt_output(f->rt, out, ctx); }

bool funk_call(funk *f, const char *name, int n, const long *args, long *result) {
  ON_THREAD(f, false);
  int g = global_lookup(intern(name));
  if (g < 0) {
    report("`%s` is not defined", name);
    return false;
  }
  if (g != ST->funk_call_checked || n != ST->funk_call_checked_n) {
    if (n < 1 || n > 8 || !int_function(CS->globals.a[g].type, n)) {
      report("funk_call needs a function from Int arguments to Int: `%s`", name);
      return false;
    }
    ST->funk_call_checked = g, ST->funk_call_checked_n = n;
  }
  atomic_store(&fk_interrupt, 0);
  CATCH_RUNTIME(return false);
  install_prelude();
  V a[8];
  for (int i = 0; i < n; i++) a[i] = FK_MKINT(args[i]);
  *result = FK_INT(fk_apply(interp_global(g), n, a));
  fk_error_jmp = NULL;
  return true;
}
