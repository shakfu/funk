/* main.c -- the command line and the REPL, over libfunk.h. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libfunk.h"

enum { M_REPL, M_RUN, M_BUILD, M_EMITC, M_CHECK, M_EVAL };
typedef struct {
  int mode;
  const char *file, *out, *expr;
  const char *opt; /* C optimization level for build */
  bool kernels;    /* emit-c --kernels */
  const char **cc_args; /* -I, -l and -L: for build, and for the interpreter's foreign-import glue */
  int ncc_args;
  const char **mod_dirs; /* -M */
  int nmod_dirs;
} Opts;

static _Noreturn void die(const char *fmt, const char *arg) {
  fflush(stdout);
  fprintf(stderr, "funk: ");
  fprintf(stderr, fmt, arg);
  fputc('\n', stderr);
  exit(1);
}

typedef struct { const char *name, *synopsis, *help; } Command;

static const Command commands[] = {
    {"repl", "repl [FILE] [-M DIR] [-I DIR] [-l LIB] [-L DIR]", "Start the REPL, after loading FILE if given. This is the default with no arguments.\n"
                            "In the REPL, :t EXPR shows a type, :l FILE loads, :{ ... :} is multi-line, :q quits.\n"
                            "  -M DIR    search DIR for imported modules, after FILE's directory; repeatable\n"
                            "  -I DIR    search DIR for the headers of foreign imports; repeatable\n"
                            "  -l LIB    link foreign imports with LIB; repeatable\n"
                            "  -L DIR    search DIR for libraries; repeatable\n"},
    {"run", "run FILE [-M DIR] [-I DIR] [-l LIB] [-L DIR]", "Interpret FILE and run its `main`. `funk FILE` does the same.\n"
                                          "  -M DIR    search DIR for imported modules, after FILE's directory; repeatable\n"
                                          "  -I DIR    search DIR for the headers of foreign imports; repeatable\n"
                                          "  -l LIB    link foreign imports with LIB; repeatable\n"
                                          "  -L DIR    search DIR for libraries; repeatable\n"},
    {"build", "build FILE [-o OUT] [-O LEVEL] [-M DIR] [-I DIR] [-l LIB] [-L DIR]",
     "Compile FILE to a native executable with $CC (default cc).\n"
     "  -M DIR    search DIR for imported modules, after FILE's directory; repeatable\n"
     "  -o OUT    the executable; default FILE without .fk\n"
     "  -O LEVEL  the C optimization level passed to $CC; default 2\n"
     "  -I DIR    search DIR for the headers of foreign imports; repeatable\n"
     "  -l LIB    link with LIB; repeatable\n"
     "  -L DIR    search DIR for libraries; repeatable\n"},
    {"emit-c", "emit-c FILE [-o OUT] [-M DIR] [--kernels]", "Write the C program that build compiles, to OUT or standard output.\n"
                                               "  -M DIR     search DIR for imported modules, after FILE's directory; repeatable\n"
                                               "  --kernels  only FILE's foreign exports, without the runtime, for a host to load; FILE need not define `main`.\n"
                                               "             The first line names the libraries to link, as `/* link: -lm */`\n"},
    {"check", "check FILE [-M DIR]", "Parse and type-check FILE, report errors and warnings, and run nothing.\n"
                            "Exits 0 if FILE compiles. FILE need not define `main`.\n"
                            "  -M DIR    search DIR for imported modules, after FILE's directory; repeatable\n"},
    {"eval", "eval EXPR", "Evaluate EXPR with the prelude in scope and print it. `funk -e EXPR` does the same.\n"},
    {"help", "help [COMMAND]", "Show this help, or COMMAND's.\n"},
};
#define NCOMMANDS (int)(sizeof commands / sizeof commands[0])

static void help(FILE *f) {
  fprintf(f, "funk %s -- a strict functional language with Haskell syntax\n\nusage:\n", funk_version());
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

static void write_text(const char *path, const char *text) {
  FILE *f = path ? fopen(path, "w") : stdout;
  if (!f) die("cannot write %s", path);
  fputs(text, f);
  if (path) fclose(f);
}

static char *default_exe(const char *file) {
  const char *base = strrchr(file, '/');
  base = base ? base + 1 : file;
  size_t n = strlen(base);
  char *s = malloc(n + 5);
  if (!s) die("%s", "out of memory");
  memcpy(s, base, n + 1);
  if (n >= 3 && strcmp(s + n - 3, ".fk") == 0) s[n - 3] = 0;
  else strcpy(s + n, ".exe");
  return s;
}

/* ------------------------------------------------------------------- REPL */

static bool eval_print(const char *src, const char *name) {
  char *shown;
  bool ok = funk_eval(src, name, &shown);
  if (shown) puts(shown);
  free(shown);
  return ok;
}

static void load(const char *path) {
  if (funk_load_file(path)) printf("loaded %s\n", path);
}

static void command(const char *line) {
  const char *arg = line + 2;
  while (*arg == ' ') arg++;
  if (line[1] == 'q') exit(0);
  if (line[1] == 't' && (line[2] == ' ' || !line[2])) {
    char *t = funk_type_of(arg, "<repl>");
    if (t) printf("%s :: %s\n", arg, t);
    free(t);
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
  char *block = NULL; /* multi-line input so far */
  size_t nblock = 0;
  bool multi = false;
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  for (;;) {
    if (tty) fputs(multi ? "  | " : "funk> ", stdout);
    fflush(stdout);
    if ((n = getline(&line, &cap, stdin)) < 0) break;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (multi) {
      if (strcmp(line, ":}") == 0) {
        multi = false;
        eval_print(block ? block : "", "<repl>");
      } else {
        block = realloc(block, nblock + (size_t)n + 2);
        if (!block) die("%s", "out of memory");
        memcpy(block + nblock, line, (size_t)n);
        nblock += (size_t)n;
        block[nblock++] = '\n';
        block[nblock] = 0;
      }
      continue;
    }
    const char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) continue;
    if (strcmp(s, ":{") == 0) {
      multi = true;
      nblock = 0;
      if (block) block[0] = 0;
      continue;
    }
    if (*s == ':') command(s);
    else eval_print(line, "<repl>");
  }
  if (tty) putchar('\n');
}

/* ------------------------------------------------------------------- main */

static int task(void *arg) {
  Opts *o = arg;
  if (!funk_init(o->cc_args, o->ncc_args)) return 1;
  for (int i = 0; i < o->nmod_dirs; i++) funk_module_dir(o->mod_dirs[i]);
  switch (o->mode) {
  case M_BUILD: {
    int rc = funk_build(o->file, o->out ? o->out : default_exe(o->file), o->opt);
    return rc < 0 ? 1 : rc; /* the C compiler's status */
  }
  case M_EMITC: {
    char *c = o->kernels ? funk_emit_kernels(o->file) : funk_emit_c(o->file);
    if (!c) return 1;
    write_text(o->out, c);
    return 0;
  }
  case M_CHECK: return funk_check_file(o->file) ? 0 : 1;
  case M_EVAL: {
    bool ok = eval_print(o->expr, "<eval>");
    fflush(stdout);
    return ok ? 0 : 1;
  }
  case M_REPL:
    if (o->file) load(o->file);
    repl();
    return 0;
  default: return funk_run_file(o->file) ? 0 : 1;
  }
}

static bool is_help(const char *a) { return strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0; }

int main(int argc, char **argv) {
  Opts o = {.mode = M_REPL, .opt = "2", .cc_args = calloc((size_t)argc, sizeof(char *)), .mod_dirs = calloc((size_t)argc, sizeof(char *))};
  if (!o.cc_args || !o.mod_dirs) die("%s", "out of memory");
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
      printf("funk %s\n", funk_version());
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
  bool build = o.mode == M_BUILD, out = build || o.mode == M_EMITC, links = build || o.mode == M_RUN || o.mode == M_REPL;
  for (; i < argc; i++) {
    const char *a = argv[i];
    bool more = i + 1 < argc;
    if (is_help(a)) {
      const Command *h = find_command(cmd);
      printf("usage: funk %s\n\n%s", h->synopsis, h->help);
      return 0;
    } else if (o.mode == M_EVAL && !o.expr) {
      o.expr = a;
    } else if (o.mode == M_EMITC && strcmp(a, "--kernels") == 0) {
      o.kernels = true;
    } else if (out && strcmp(a, "-o") == 0) {
      if (!more) usage_error(cmd, "`-o` needs a file name", NULL);
      o.out = argv[++i];
    } else if (o.mode != M_EVAL && strncmp(a, "-M", 2) == 0) {
      if (!a[2] && !more) usage_error(cmd, "`-M` needs a directory", NULL);
      o.mod_dirs[o.nmod_dirs++] = a[2] ? a + 2 : argv[++i];
    } else if (build && strncmp(a, "-O", 2) == 0) {
      o.opt = a[2] ? a + 2 : more ? argv[++i] : NULL;
      if (!o.opt || strspn(o.opt, "0123sz") != strlen(o.opt) || strlen(o.opt) != 1)
        usage_error(cmd, "`-O` takes one of 0, 1, 2, 3, s, z", NULL);
    } else if (links && (strncmp(a, "-l", 2) == 0 || strncmp(a, "-L", 2) == 0 || strncmp(a, "-I", 2) == 0)) {
      if (!a[2] && !more) usage_error(cmd, "`%s` needs an argument", a);
      if (a[2]) {
        o.cc_args[o.ncc_args++] = a;
      } else { /* -I DIR as -IDIR */
        const char *v = argv[++i];
        char *j = malloc(strlen(a) + strlen(v) + 1);
        if (!j) die("%s", "out of memory");
        o.cc_args[o.ncc_args++] = strcat(strcpy(j, a), v);
      }
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
  return funk_main(task, &o);
}
