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

/* Imported modules (modules.md): each file once, keyed by realpath, its
   chunk compiled before the files that import it. */
typedef struct { const char *real, *file, *prefix; bool done; Exports *ex; } Mod; /* file: as found, for messages */
static VEC(Mod) mods;
static VEC(Chunk) mod_chunks;
static int mods_installed; /* in the interpreter */
static Strs mod_dirs;      /* -M DIR */
static ImportInfo *last_imps; /* the program's imports, resolved by the last prepare */
static int nlast_imps;
static VEC(ImportInfo) repl_imps; /* in scope at the prompt: `import` inputs and :l files' imports */

/* The REPL's imports as Import lines, for the parser's qualified tokens. */
static Import *repl_lines(void) {
  Import *ims = xmalloc((size_t)repl_imps.n * sizeof *ims + 1);
  for (int i = 0; i < repl_imps.n; i++) ims[i] = *repl_imps.a[i].im;
  return ims;
}

/* The last program's imports into the REPL's scope; a qualifier bound
   before is rebound. */
static void adopt_imports(void) {
  for (int i = 0; i < nlast_imps; i++) {
    int k = 0;
    while (k < repl_imps.n && repl_imps.a[k].im->qual != last_imps[i].im->qual) k++;
    if (k == repl_imps.n) VPUSH(repl_imps, last_imps[i]);
    else repl_imps.a[k] = last_imps[i];
  }
}

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

static Module *begin_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = cerr_jmp;
  Module *volatile m = NULL;
  if (!setjmp(jb)) {
    cerr_jmp = &jb;
    m = parse_begin(src, len);
  }
  cerr_jmp = saved;
  return m;
}

static Expr *parse_expr_caught(const char *src, size_t len) {
  jmp_buf jb, *saved = cerr_jmp;
  Expr *volatile e = NULL;
  if (!setjmp(jb)) {
    cerr_jmp = &jb;
    e = parse_expr_text(src, len, repl_lines(), repl_imps.n);
    Bind *b = NEW(Bind); /* qualified through a module holding e */
    *b = (Bind){.name = intern("$q"), .e = e};
    if (repl_imps.n) qualify_module(&(Module){.binds = &b, .nb = 1}, NULL, repl_imps.a, repl_imps.n);
    e = b->e;
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

void funk_module_dir(const char *dir) { VPUSH(mod_dirs, dir); }

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
  for (int i = 0; i < mod_dirs.n; i++) VPUSH(dirs, mod_dirs.a[i]);
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
  for (int i = 0; i < mods.n; i++) {
    if (mods.a[i].real != real) continue;
    if (mods.a[i].done) return i;
    SB chain = {0}; /* the modules still loading, from this one, are the cycle */
    for (int j = i; j < mods.n; j++) sb_printf(&chain, "%s -> ", mods.a[j].prefix);
    cerror(im->line, im->col, "import cycle: %s%s", chain.s, mods.a[i].prefix);
  }
  const char *dot = strrchr(im->path, '.'), *prefix = intern(dot ? dot + 1 : im->path);
  for (int i = 0; i < mods.n; i++)
    if (mods.a[i].prefix == prefix)
      cerror(im->line, im->col, "two modules are named `%s`: %s, and %s imported earlier", prefix, found, mods.a[i].file);
  VPUSH(mods, ((Mod){real, found, prefix, false, NULL}));
  int me = mods.n - 1;
  size_t len;
  char *src = read_file(found, &len);
  if (!src) cerror(im->line, im->col, "cannot read %s", found);
  cur_file = found;
  Module *m = parse_begin(src, len);
  if (m->name) {
    const char *hd = strrchr(m->name, '.');
    if (strcmp(hd ? hd + 1 : m->name, prefix)) cerror(m->line, m->col, "the module is named `%s`, but it is imported as `%s`", m->name, im->path);
  }
  Chunk c;
  Exports *ex = prepare(m, found, prefix);
  if (!ex) return -1;
  cur_file = found;
  const char *cm = cur_module;
  cur_module = prefix;
  bool ok = compile_chunk(m, &c);
  cur_module = cm;
  if (!ok) return -1;
  VPUSH(mod_chunks, c);
  mods.a[me].done = true;
  mods.a[me].ex = ex;
  cur_file = from;
  return me;
}

/* Compile m's imports, parse the rest of m, then qualify it: with prefix
   for a module, NULL for the program. What m exports, or NULL after an
   error. */
static Exports *prepare(Module *m, const char *file, const char *prefix) {
  static Exports none;
  if (!m->nimports && !prefix) {
    parse_finish(m);
    nlast_imps = 0;
    return &none;
  }
  ImportInfo *imps = xmalloc((size_t)m->nimports * sizeof *imps + 1);
  for (int i = 0; i < m->nimports; i++) {
    cur_file = file;
    Import *im = &m->imports[i];
    if (im->base) { /* the prelude stands in for it: names checked, none added (modules.md) */
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
    imps[i] = (ImportInfo){&m->imports[i], mods.a[k].prefix, mods.a[k].ex};
  }
  cur_file = file;
  parse_finish(m);
  if (!prefix) {
    last_imps = imps;
    nlast_imps = m->nimports;
  }
  return qualify_module(m, prefix, imps, m->nimports);
}

static bool prepare_caught(Module *m, const char *file) {
  jmp_buf jb, *saved = cerr_jmp;
  const char *cf = cur_file;
  int n0 = mods.n;
  volatile bool ok = false;
  if (!setjmp(jb)) {
    cerr_jmp = &jb;
    ok = prepare(m, file, NULL) != NULL;
  }
  cerr_jmp = saved;
  cur_file = cf;
  if (!ok) { /* forget the modules left half-loaded; compiled ones stay */
    int k = n0;
    for (int i = n0; i < mods.n; i++)
      if (mods.a[i].done) mods.a[k++] = mods.a[i];
    mods.n = k;
  }
  return ok;
}

/* The rest of a REPL declaration, qualified with the REPL's imports. */
static bool finish_caught(Module *m) {
  jmp_buf jb, *saved = cerr_jmp;
  volatile bool ok = false;
  if (!setjmp(jb)) {
    cerr_jmp = &jb;
    parse_finish(m);
    if (repl_imps.n) qualify_module(m, NULL, repl_imps.a, repl_imps.n);
    ok = true;
  }
  cerr_jmp = saved;
  return ok;
}

static void install_modules(void) {
  for (; mods_installed < mod_chunks.n; mods_installed++) interp_install(&mod_chunks.a[mods_installed]);
}

static bool compile_file(const char *path, Chunk *out) {
  size_t len;
  char *src = read_file(path, &len);
  if (!src) {
    report("cannot read %s", path);
    return false;
  }
  cur_file = path;
  Module *m = begin_caught(src, len);
  return m && prepare_caught(m, path) && compile_chunk(m, out);
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
  install_modules();
  interp_install(&c);
  fk_error_jmp = NULL;
  fflush(stdout);
  return true;
}

char *funk_emit_c(const char *path) {
  Chunk c;
  if (!compile_program(path, &c)) return NULL;
  int n = mod_chunks.n + 2;
  Chunk *all = xmalloc((size_t)n * sizeof *all);
  all[0] = prelude;
  copy_n(all + 1, mod_chunks.a, (size_t)mod_chunks.n * sizeof *all);
  all[n - 1] = c;
  return cgen_program(all, n);
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
    install_modules();
    interp_install(&c);
    adopt_imports();
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
      m->nimports = repl_imps.n;
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
