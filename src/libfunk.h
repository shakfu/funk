/* libfunk.h -- the library's api for drivers such as cli/main.c.
   Internal: no stability promise between releases. It includes nothing of
   the compiler's own headers, so a driver sees only these functions.

   One session per process, used from one thread: the compiler's tables
   and the heap are global. Every function but funk_version and funk_main
   runs inside funk_main's callback, on the large stack the runtime needs.
   Errors are reported on stderr; functions return false (or NULL) after
   one, and never exit. */
#ifndef LIBFUNK_H
#define LIBFUNK_H

#include <stdbool.h>

const char *funk_version(void);

/* Run fn(arg) on the runtime's stack; returns what fn returns, or 1 if
   the stack cannot be made. */
int funk_main(int (*fn)(void *), void *arg);

/* Compile the prelude. cc_args (-I, -l, -L) are for foreign-import glue
   and funk_build; the strings must outlive the session. */
bool funk_init(const char *const *cc_args, int ncc_args);

/* Search dir for imported modules, after the importing file's own
   directory; in the order given (modules.md). The string must outlive
   the session. */
void funk_module_dir(const char *dir);

/* Parse and type-check path, running nothing; it need not define `main`. */
bool funk_check_file(const char *path);

/* Interpret path and run its `main`. */
bool funk_run_file(const char *path);

/* The C program for path, which must define `main`; NULL on error. */
char *funk_emit_c(const char *path);

/* Compile path to the executable exe with $CC at C optimization level opt
   ("0" to "3", "s", "z"); the compiler's exit status, or -1 before it runs. */
int funk_build(const char *path, const char *exe, const char *opt);

/* Load path into the session, as the REPL's :l does. */
bool funk_load_file(const char *path);

/* Evaluate src, a declaration or an expression, as REPL input named name
   in messages. *shown is the expression's value shown, or NULL for a
   declaration or `()`; the caller frees it. */
bool funk_eval(const char *src, const char *name, char **shown);

/* The type of the expression src, named name in messages; the caller
   frees it. NULL on error. */
char *funk_type_of(const char *src, const char *name);

#endif
