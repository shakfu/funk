/* libfunk.h -- the library's api for drivers such as cli/main.c.
   Internal: no stability promise between releases. It includes nothing of
   the compiler's own headers, so a driver sees only these functions.

   A session, funk, owns a thread on the large stack the runtime needs;
   funk_run runs a host's function there. The other functions taking a
   funk run on that thread, inside funk_run, but for funk_free,
   funk_interrupt, funk_output and funk_on_fatal. Sessions share nothing
   but the process: each has its own heap, compiler and thread, up to 256
   at once (docs/dev/embedding.md, phase 2). Errors are reported on stderr;
   functions return false (or NULL) after one, and never exit. */
#ifndef LIBFUNK_H
#define LIBFUNK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility push(default) /* libfunk hides everything else */

const char *funk_version(void);

typedef struct funk funk;

/* heap: the heap's limit, beyond which allocation is the runtime error
   "out of memory" after a full collection; threshold: the allocation after
   which the first collection starts, kept at most a quarter of the limit;
   stack: beyond which recursion is the runtime error "stack overflow". In
   bytes; 0 keeps a default: 64 GiB of address space, 128 MiB, 4 GiB. The
   heap is at least 4 MiB, the stack 32 MiB. cc_args (-I, -l, -L) are for
   foreign-import glue and funk_build; the strings must outlive the session. */
typedef struct {
  size_t heap, threshold, stack;
  const char *const *cc_args;
  int ncc_args;
} funk_config;

/* A session: starts its thread and compiles the prelude there. c may be
   NULL, for the defaults. NULL on error. */
funk *funk_new(const funk_config *c);

/* Run fn(f, arg) on f's thread and return what it returns. From another
   thread, it waits for the job before it, then for fn. */
int funk_run(funk *f, int (*fn)(funk *f, void *arg), void *arg);

/* Stops f's thread, after its job; not from that thread. The replaced
   signal handlers are installed again. */
void funk_free(funk *f);

/* fn runs on an error funk cannot recover from, such as malloc failing;
   then the process exits. It may save the host's work; it must not call
   funk. */
void funk_on_fatal(funk *f, void (*fn)(void *ctx, const char *msg), void *ctx);

/* Search dir for imported modules, after the importing file's own
   directory; in the order given (modules.md). The string must outlive
   the session. */
void funk_module_dir(funk *f, const char *dir);

/* Foreign imports of the C name `name`, a function or variable, use addr
   rather than a symbol of the process, which a plugin does not export.
   The import's header still declares it. Before the code that imports it
   loads; the string must outlive the session. False if name is not a C
   identifier or addr is NULL. */
bool funk_provide(funk *f, const char *name, void *addr);

/* Glue linked into the host, from `funk emit-c --glue`: a foreign import
   whose key is in glue[0..n) uses the wrapper given there, and needs no C
   compiler. An import without a matching key, or whose type, C name or
   header has changed since, is compiled as before. Before the code that
   imports it loads. */
typedef struct {
  uint64_t key;
  void *code;
} funk_glue_entry;
void funk_glue(funk *f, const funk_glue_entry *glue, int n);

/* Parse and type-check path, running nothing; it need not define `main`. */
bool funk_check_file(funk *f, const char *path);

/* Interpret path and run its `main`. */
bool funk_run_file(funk *f, const char *path);

/* The C program for path, which must define `main`; NULL on error. */
char *funk_emit_c(funk *f, const char *path);

/* path's foreign exports alone, as C without the runtime, for a host to
   load (docs/dev/audio.md, D); path need not define `main`. NULL on error. */
char *funk_emit_kernels(funk *f, const char *path);

/* The glue of the foreign imports of path and what it imports, as C to link
   with libfunk, defining funk_glue_table and funk_glue_count for funk_glue;
   path need not define `main`. NULL on an error, reported. */
char *funk_emit_glue(funk *f, const char *path);

/* Compile path to the executable exe with $CC at C optimization level opt
   ("0" to "3", "s", "z"); the compiler's exit status, or -1 before it runs. */
int funk_build(funk *f, const char *path, const char *exe, const char *opt);

/* Load path into the session, as the REPL's :l does. */
bool funk_load_file(funk *f, const char *path);

/* Evaluate src, a declaration or an expression, as REPL input named name
   in messages. *shown is the expression's value shown, or NULL for a
   declaration or `()`; the caller frees it. */
bool funk_eval(funk *f, const char *src, const char *name, char **shown);

/* From any thread: f's running funk_run_file, funk_load_file, funk_eval or
   funk_call stops with the runtime error "interrupted". Interpreted code
   checks at each call, so a loop stops; a long primitive, such as sorting,
   finishes first. A call that starts afterwards clears it. */
void funk_interrupt(funk *f);

/* Send what funk writes to out instead of stdout and stderr: stream 1 is
   the program's output (putStr, print), 2 its messages (compile and runtime
   errors, warnings, trace), one call per message. NULL restores them.
   Output of the C compiler that builds foreign-import glue is not included. */
void funk_output(funk *f, void (*out)(void *ctx, int stream, const char *s, size_t n), void *ctx);

/* Call the global name, the newest definition of it, with n Int
   arguments (1 to 8); its type must be Int -> ... -> Int, with `->` or
   `->>` arrows. Unlike funk_eval, it compiles nothing, so a host may call
   it at a fixed rate for as long as it runs. */
bool funk_call(funk *f, const char *name, int n, const long *args, long *result);

/* The type of the expression src, named name in messages; the caller
   frees it. NULL on error. */
char *funk_type_of(funk *f, const char *src, const char *name);

#pragma GCC visibility pop

#endif
