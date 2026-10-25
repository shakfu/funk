/* main.c -- command line driver and REPL. */
#define _DEFAULT_SOURCE
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "funk.h"

extern char **environ;

#ifndef FUNK_VERSION
#define FUNK_VERSION "dev" /* CMake defines it from the project version */
#endif

enum { M_REPL, M_RUN, M_BUILD, M_EMITC, M_CHECK, M_EVAL };
typedef struct {
  int mode;
  const char *file, *out, *expr;
  const char *opt; /* C optimization level for build */
  VEC(const char *) cc_args; /* -l and -L for build */
} Opts;

static Chunk prelude;

static _Noreturn void die(const char *fmt, const char *arg) {
  fflush(stdout);
  fprintf(stderr, "funk: ");
  fprintf(stderr, fmt, arg);
  fputc('\n', stderr);
  exit(1);
}

typedef struct { const char *name, *synopsis, *help; } Command;

static const Command commands[] = {
    {"repl", "repl [FILE]", "Start the REPL, after loading FILE if given. This is the default with no arguments.\n"
                            "In the REPL, :t EXPR shows a type, :l FILE loads, :{ ... :} is multi-line, :q quits.\n"},
    {"run", "run FILE", "Interpret FILE and run its `main`. `funk FILE` does the same.\n"},
    {"build", "build FILE [-o OUT] [-O LEVEL] [-l LIB] [-L DIR]",
     "Compile FILE to a native executable with $CC (default cc).\n"
     "  -o OUT    the executable; default FILE without .fk\n"
     "  -O LEVEL  the C optimization level passed to $CC; default 2\n"
     "  -l LIB    link with LIB; repeatable\n"
     "  -L DIR    search DIR for libraries; repeatable\n"},
    {"emit-c", "emit-c FILE [-o OUT]", "Write the C program that build compiles, to OUT or standard output.\n"},
    {"check", "check FILE", "Parse and type-check FILE, report errors and warnings, and run nothing.\n"
                            "Exits 0 if FILE compiles. FILE need not define `main`.\n"},
    {"eval", "eval EXPR", "Evaluate EXPR with the prelude in scope and print it. `funk -e EXPR` does the same.\n"},
    {"help", "help [COMMAND]", "Show this help, or COMMAND's.\n"},
};
#define NCOMMANDS (int)(sizeof commands / sizeof commands[0])

static void help(FILE *f) {
  fputs("funk " FUNK_VERSION " -- a strict functional language with Haskell syntax\n\nusage:\n", f);
  for (int i = 0; i < NCOMMANDS; i++) fprintf(f, "  funk %s\n", commands[i].synopsis);
  fputs("  funk FILE              same as funk run FILE\n"
        "  funk -e EXPR           same as funk eval EXPR\n"
        "  funk -h | --help       this help\n"
        "  funk -V | --version    the version\n"
        "\nfunk help COMMAND shows a command's options.\n",
        f);
}

static const Command *find_command(const char *name) {
  for (int i = 0; i < NCOMMANDS; i++)
    if (strcmp(commands[i].name, name) == 0) return &commands[i];
  return NULL;
}

/* A usage error: the message, then where to look. */
static _Noreturn void usage_error(const char *cmd, const char *fmt, const char *arg) {
  fprintf(stderr, "funk%s%s: ", cmd ? " " : "", cmd ? cmd : "");
  fprintf(stderr, fmt, arg);
  fprintf(stderr, "\nsee: funk help%s%s\n", cmd ? " " : "", cmd ? cmd : "");
  exit(2);
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

static bool compile_file(const char *path, Chunk *out) {
  size_t len;
  char *src = read_file(path, &len);
  if (!src) die("cannot read %s", path);
  cur_file = path;
  Module *m = parse_caught(src, len);
  return m && compile_chunk(m, out);
}

static int main_global(int first) {
  int g = global_lookup(intern("main"));
  if (g < first) die("%s: no `main` defined", cur_file);
  return g;
}

/* ------------------------------------------------------------------ build */

static void write_text(const char *path, const char *text) {
  FILE *f = path ? fopen(path, "w") : stdout;
  if (!f) die("cannot write %s", path);
  fputs(text, f);
  if (path) fclose(f);
}

static int run_cc(const char *csrc, const char *exe, Opts *o) {
  const char *cc = getenv("CC");
  if (!cc || !*cc) cc = "cc";
  VEC(char *) argv = {0};
  char *ccw = strdup(cc);
  for (char *w = strtok(ccw, " "); w; w = strtok(NULL, " ")) VPUSH(argv, w);
  char *fixed[] = {xsprintf("-O%s", o->opt), "-o", (char *)exe, (char *)csrc, NULL};
  for (int i = 0; fixed[i]; i++) VPUSH(argv, fixed[i]);
  for (int i = 0; i < o->cc_args.n; i++) VPUSH(argv, (char *)o->cc_args.a[i]);
  static const char *const runtime_libs[] = {"-lm", "-lpthread"};
  for (int i = 0; i < 2; i++) {
    bool given = false; /* the linker warns about a library named twice */
    for (int j = 0; j < o->cc_args.n; j++) given |= strcmp(o->cc_args.a[j], runtime_libs[i]) == 0;
    if (!given) VPUSH(argv, (char *)runtime_libs[i]);
  }
  VPUSH(argv, NULL);
  pid_t pid;
  if (posix_spawnp(&pid, argv.a[0], NULL, NULL, argv.a, environ)) die("cannot run %s", argv.a[0]);
  int status;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static char *default_exe(const char *file) {
  const char *base = strrchr(file, '/');
  base = base ? base + 1 : file;
  char *s = xsprintf("%s", base);
  char *dot = strrchr(s, '.');
  if (dot && strcmp(dot, ".fk") == 0) *dot = 0;
  else s = xsprintf("%s.exe", s);
  return s;
}

static void build(Opts *o) {
  int first = globals.n;
  Chunk user;
  if (!compile_file(o->file, &user)) exit(1);
  main_global(first);
  char *c = cgen_program((Chunk[]){prelude, user}, 2);
  if (o->mode == M_EMITC) {
    write_text(o->out, c);
    exit(0);
  }
  const char *dir = getenv("TMPDIR");
  char *tmp = xsprintf("%s/funkXXXXXX.c", dir && *dir ? dir : "/tmp");
  int fd = mkstemps(tmp, 2);
  if (fd < 0) die("cannot create %s", tmp);
  FILE *f = fdopen(fd, "w");
  fputs(c, f);
  fclose(f);
  int rc = run_cc(tmp, o->out ? o->out : default_exe(o->file), o);
  unlink(tmp);
  exit(rc);
}

/* ------------------------------------------------------------------- REPL */

/* Print a REPL result with its Show instance. Without one, print it
   structurally; a function prints as `<function> :: type`. */
static void print_result(Bind *it) {
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
    char *s = fk_cstring(interp_global(b->gidx), NULL);
    puts(s);
    free(s);
    return;
  }
  V v = interp_global(it->gidx);
  if (FK_ISPTR(v) && FK_HDR(v)->kind != K_CON && FK_HDR(v)->kind != K_FLOAT) {
    printf("<function> :: %s\n", type_str(globals.a[it->gidx].type));
    return;
  }
  char *s = fk_show_cstr(v);
  puts(s);
  free(s);
}

static bool eval_failed; /* the last eval_input reported an error */

static void eval_input(const char *src) {
  size_t len = strlen(src);
  jmp_buf rt;
  eval_failed = true;
  fk_error_jmp = &rt;
  if (setjmp(rt)) { fk_error_jmp = NULL; return; }
  int decl = is_decl_caught(src, len);
  if (decl < 0) { fk_error_jmp = NULL; return; }
  Chunk c;
  if (decl) {
    int mark = undo_mark(); /* fixities installed by the parser */
    Module *m = parse_caught(src, len);
    if (m && compile_chunk(m, &c)) {
      interp_install(&c);
      eval_failed = false;
    } else {
      undo_to(mark);
    }
  } else {
    Expr *e = parse_expr_caught(src, len);
    if (!e) { fk_error_jmp = NULL; return; }
    Bind *b = NEW(Bind); /* bound as $it so `it` in e means the previous result */
    b->name = intern("$it");
    b->e = e;
    b->line = e->line;
    b->col = e->col;
    Module m = {.binds = &b, .nb = 1};
    if (compile_chunk(&m, &c)) {
      interp_install(&c);
      global_alias(intern("it"), b->gidx);
      V v = interp_global(b->gidx);
      if (v != fk_unit) print_result(b);
      eval_failed = false;
    }
  }
  fk_error_jmp = NULL;
}

static void load(const char *path) {
  jmp_buf rt;
  Chunk c;
  int mark = undo_mark();
  fk_error_jmp = &rt;
  if (!setjmp(rt)) {
    if (compile_file(path, &c)) {
      interp_install(&c);
      printf("loaded %s\n", path);
    } else {
      undo_to(mark);
    }
  }
  fk_error_jmp = NULL;
  cur_file = "<repl>";
}

static void command(const char *line) {
  const char *arg = line + 2;
  while (*arg == ' ') arg++;
  if (line[1] == 'q') exit(0);
  if (line[1] == 't' && (line[2] == ' ' || !line[2])) {
    Expr *e = parse_expr_caught(arg, strlen(arg));
    const char *t = e ? infer_type_text(e) : NULL;
    if (t) printf("%s :: %s\n", arg, t);
    return;
  }
  if (line[1] == 'l' && (line[2] == ' ' || !line[2])) {
    load(arg);
    return;
  }
  printf("commands: :t EXPR (type), :l FILE (load), :{ ... :} (multi-line), :q (quit)\n");
}

static void repl(void) {
  bool tty = isatty(0);
  if (tty) puts("funk REPL. :t EXPR shows a type, :{ starts multi-line input, :q quits.");
  SB block = {0};
  bool multi = false;
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  for (;;) {
    if (tty) fputs(multi ? "  | " : "funk> ", stdout);
    fflush(stdout);
    if ((n = getline(&line, &cap, stdin)) < 0) break;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    cur_file = "<repl>";
    if (multi) {
      if (strcmp(line, ":}") == 0) {
        multi = false;
        eval_input(block.s ? block.s : "");
      } else {
        sb_puts(&block, line);
        sb_puts(&block, "\n");
      }
      continue;
    }
    const char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) continue;
    if (strcmp(s, ":{") == 0) {
      multi = true;
      block.n = 0;
      if (block.s) block.s[0] = 0;
      continue;
    }
    if (*s == ':') command(s);
    else eval_input(line);
  }
  if (tty) putchar('\n');
}

/* ------------------------------------------------------------------- main */

static void task(void *arg) {
  Opts *o = arg;
  compiler_init();
  cur_file = "<prelude>";
  char *src = xsprintf("%s\n%s", fk_prelude_src, builtin_instances_src());
  Module *m = parse_caught(src, strlen(src));
  if (!m || !compile_chunk(m, &prelude)) die("%s", "internal error: prelude failed to compile");
  if (o->mode == M_BUILD || o->mode == M_EMITC) build(o);
  if (o->mode == M_CHECK) {
    Chunk user;
    exit(compile_file(o->file, &user) ? 0 : 1);
  }
  interp_install(&prelude);
  if (o->mode == M_EVAL) {
    cur_file = "<eval>";
    eval_input(o->expr);
    fflush(stdout);
    exit(eval_failed ? 1 : 0);
  }
  if (o->mode == M_REPL) {
    if (o->file) load(o->file);
    repl();
    exit(0);
  }
  int first = globals.n;
  Chunk user;
  if (!compile_file(o->file, &user)) exit(1);
  main_global(first);
  interp_install(&user);
  fflush(stdout);
  exit(0);
}

static bool is_help(const char *a) { return strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0; }

int main(int argc, char **argv) {
  Opts o = {.mode = M_REPL, .opt = "2"};
  static const int modes[] = {M_REPL, M_RUN, M_BUILD, M_EMITC, M_CHECK, M_EVAL};
  const char *cmd = NULL;
  int i = 1;
  if (i < argc) {
    const char *a = argv[i];
    const Command *c = find_command(a);
    if (is_help(a)) {
      help(stdout);
      return 0;
    }
    if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
      puts("funk " FUNK_VERSION);
      return 0;
    }
    if (strcmp(a, "-e") == 0) {
      a = "eval";
      c = find_command(a);
    }
    if (c && strcmp(c->name, "help") == 0) {
      if (i + 1 >= argc) {
        help(stdout);
        return 0;
      }
      const Command *h = find_command(argv[i + 1]);
      if (!h) usage_error(NULL, "unknown command `%s`", argv[i + 1]);
      printf("usage: funk %s\n\n%s", h->synopsis, h->help);
      return 0;
    }
    if (c) {
      cmd = c->name;
      o.mode = modes[c - commands];
      i++;
    } else if (a[0] == '-') {
      usage_error(NULL, "unknown option `%s`", a);
    } else if (!strchr(a, '.') && access(a, F_OK) != 0) {
      usage_error(NULL, "unknown command `%s`", a); /* not a command, and no such file */
    } else {
      cmd = "run";
      o.mode = M_RUN;
    }
  }
  bool build = o.mode == M_BUILD, out = build || o.mode == M_EMITC;
  for (; i < argc; i++) {
    const char *a = argv[i];
    bool more = i + 1 < argc;
    if (is_help(a)) {
      const Command *h = find_command(cmd);
      printf("usage: funk %s\n\n%s", h->synopsis, h->help);
      return 0;
    } else if (o.mode == M_EVAL && !o.expr) {
      o.expr = a;
    } else if (out && strcmp(a, "-o") == 0) {
      if (!more) usage_error(cmd, "`-o` needs a file name", NULL);
      o.out = argv[++i];
    } else if (build && strncmp(a, "-O", 2) == 0) {
      o.opt = a[2] ? a + 2 : more ? argv[++i] : NULL;
      if (!o.opt || strspn(o.opt, "0123sz") != strlen(o.opt) || strlen(o.opt) != 1)
        usage_error(cmd, "`-O` takes one of 0, 1, 2, 3, s, z", NULL);
    } else if (build && (strncmp(a, "-l", 2) == 0 || strncmp(a, "-L", 2) == 0)) {
      if (!a[2] && !more) usage_error(cmd, "`%s` needs an argument", a);
      VPUSH(o.cc_args, a[2] ? a : xsprintf("%s%s", a, argv[++i]));
    } else if (a[0] == '-' && a[1]) {
      usage_error(cmd, "unknown option `%s`", a);
    } else if (!o.file && o.mode != M_EVAL) {
      o.file = a;
    } else {
      usage_error(cmd, "unexpected argument `%s`", a);
    }
  }
  if (o.mode == M_EVAL && !o.expr) usage_error(cmd, "missing EXPR", NULL);
  if (o.mode != M_REPL && o.mode != M_EVAL && !o.file) usage_error(cmd, "missing FILE", NULL);
  if (o.file && access(o.file, R_OK) != 0) die("cannot read %s", o.file);
  return fk_run(task, &o);
}
