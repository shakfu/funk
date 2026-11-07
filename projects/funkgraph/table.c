/* table.c -- read-only sample data for kernels (graph.h). Tables are shared
   across graphs and plugin instances, by file and channel or by content, and
   freed with the last graph that reads them. Host threads only: the audio
   thread sees a pointer that a graph keeps alive. */
#define _DEFAULT_SOURCE
#include "table.h"

#include <dirent.h>
#include <pthread.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct funk_table {
  float *data;
  int64_t frames;
  double rate;
  char *path; /* NULL for values */
  int channel;
  long long size, mtime_s, mtime_ns; /* the file as it was read */
  int refs;   /* under lock */
  funk_table *next;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static funk_table *tables;
static _Thread_local const char *last_error;

const char *funk_table_error(void) { return last_error ? last_error : "no error"; }

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

const char *funk_sample_path(const char *dir, const char *name, int index) {
  static _Thread_local char out[4096];
  if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
    last_error = "a sample name is one folder's name";
    return "";
  }
  char folder[4096];
  if (snprintf(folder, sizeof folder, "%s/%s", dir, name) >= (int)sizeof folder) {
    last_error = "the sample folder's path is too long";
    return "";
  }
  DIR *d = opendir(folder);
  if (!d) {
    last_error = "no such sample folder";
    return "";
  }
  char **files = NULL;
  size_t n = 0, cap = 0;
  for (struct dirent *e; (e = readdir(d));) {
    size_t len = strlen(e->d_name);
    if (e->d_name[0] == '.' || len < 5 || strcasecmp(e->d_name + len - 4, ".wav")) continue;
    if (n == cap) {
      char **more = realloc(files, (cap = cap ? 2 * cap : 16) * sizeof *files);
      if (!more) break;
      files = more;
    }
    if (!(files[n] = strdup(e->d_name))) break;
    n++;
  }
  closedir(d);
  const char *result = "";
  if (!n) last_error = "no WAV files in the sample folder";
  else {
    qsort(files, n, sizeof *files, by_name);
    long i = index % (long)n;
    if (i < 0) i += (long)n;
    if (snprintf(out, sizeof out, "%s/%s", folder, files[i]) < (int)sizeof out) result = out;
    else last_error = "the sample's path is too long";
  }
  for (size_t i = 0; i < n; i++) free(files[i]);
  free(files);
  return result;
}

static uint32_t u16le(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32le(const unsigned char *p) { return u16le(p) | u16le(p + 2) << 16; }

/* One sample in -1 to 1; format 1 is integer, 3 is float. */
static float sample(const unsigned char *p, uint32_t format, uint32_t bytes) {
  if (format == 3) {
    if (bytes == 4) {
      uint32_t u = u32le(p);
      float f;
      memcpy(&f, &u, sizeof f);
      return f;
    }
    uint64_t u = (uint64_t)u32le(p) | (uint64_t)u32le(p + 4) << 32;
    double d;
    memcpy(&d, &u, sizeof d);
    return (float)d;
  }
  switch (bytes) {
    case 1: return (float)((int)p[0] - 128) / 128.0f;
    case 2: return (float)(int16_t)u16le(p) / 32768.0f;
    case 3: return (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.0f;
    default: return (float)((double)(int32_t)u32le(p) / 2147483648.0);
  }
}

/* The file's bytes are untrusted: every offset is checked against n. */
static const char *decode(const unsigned char *b, size_t n, int channel, funk_table *t) {
  if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return "not a WAV file";
  const unsigned char *fmt = NULL, *data = NULL;
  size_t fmt_n = 0, data_n = 0;
  for (size_t at = 12; n - at >= 8;) {
    size_t len = u32le(b + at + 4), body = at + 8, left = n - body;
    if (!memcmp(b + at, "fmt ", 4) && len <= left) {
      fmt = b + body;
      fmt_n = len;
    } else if (!memcmp(b + at, "data", 4)) {
      data = b + body;
      data_n = len < left ? len : left; /* a stream's header may overstate it */
      break;
    }
    if (len >= left) break;
    at = body + len + (len & 1);
    if (at > n) break;
  }
  if (!fmt || fmt_n < 16) return "no fmt chunk";
  if (!data) return "no data chunk";
  uint32_t format = u16le(fmt), channels = u16le(fmt + 2), rate = u32le(fmt + 4), align = u16le(fmt + 12), bits = u16le(fmt + 14);
  if (format == 0xFFFE && fmt_n >= 26) format = u16le(fmt + 24); /* extensible: the subformat */
  uint32_t bytes = bits / 8;
  if (format != 1 && format != 3) return "not PCM or float";
  if (!channels || !rate || bits % 8) return "bad format";
  if (format == 1 ? bytes < 1 || bytes > 4 : bytes != 4 && bytes != 8) return "unsupported sample size";
  if (align != channels * bytes) return "bad frame size";
  size_t frames = data_n / align;
  if (frames > INT32_MAX) return "too long";
  int c = channel < (int)channels ? channel : (int)channels - 1; /* a mono file plays on every channel */
  t->data = malloc((frames ? frames : 1) * sizeof(float));
  if (!t->data) return "out of memory";
  for (size_t i = 0; i < frames; i++) t->data[i] = sample(data + i * align + (size_t)c * bytes, format, bytes);
  t->frames = (int64_t)frames;
  t->rate = rate;
  return NULL;
}

static funk_table *load(const char *path, int channel, const struct stat *st) {
  if (st->st_size > (long long)1 << 31) {
    last_error = "too long";
    return NULL;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    last_error = "cannot open";
    return NULL;
  }
  size_t cap = (size_t)st->st_size;
  unsigned char *b = malloc(cap ? cap : 1);
  size_t n = b ? fread(b, 1, cap, f) : 0;
  fclose(f);
  funk_table *t = calloc(1, sizeof *t);
  char *name = malloc(strlen(path) + 1);
  const char *why = !b || !t || !name ? "out of memory" : decode(b, n, channel, t);
  free(b);
  if (why) {
    if (t) free(t->data);
    free(t);
    free(name);
    last_error = why;
    return NULL;
  }
  t->path = strcpy(name, path);
  return t;
}

#ifdef __APPLE__
#define MTIME_NS(st) ((st).st_mtimespec.tv_nsec)
#else
#define MTIME_NS(st) ((st).st_mtim.tv_nsec)
#endif

funk_table *funk_table_file(const char *path, int channel) {
  struct stat st;
  if (!path || channel < 0) {
    last_error = "bad argument";
    return NULL;
  }
  if (stat(path, &st) || !S_ISREG(st.st_mode)) {
    last_error = "cannot open";
    return NULL;
  }
  long long ns = (long long)MTIME_NS(st);
  pthread_mutex_lock(&lock);
  for (funk_table *t = tables; t; t = t->next)
    if (t->path && t->channel == channel && !strcmp(t->path, path) && t->size == (long long)st.st_size && t->mtime_s == (long long)st.st_mtime && t->mtime_ns == ns) {
      t->refs++;
      pthread_mutex_unlock(&lock);
      return t;
    }
  pthread_mutex_unlock(&lock);
  funk_table *t = load(path, channel, &st); /* outside the lock: other instances may be loading */
  if (!t) return NULL;
  t->channel = channel;
  t->size = (long long)st.st_size;
  t->mtime_s = (long long)st.st_mtime;
  t->mtime_ns = ns;
  t->refs = 1;
  pthread_mutex_lock(&lock);
  t->next = tables;
  tables = t;
  pthread_mutex_unlock(&lock);
  return t;
}

funk_table *funk_table_values(const double *values, int64_t frames) {
  if (frames < 0 || frames > INT32_MAX || (frames && !values)) {
    last_error = "bad argument";
    return NULL;
  }
  float *d = malloc((size_t)(frames ? frames : 1) * sizeof *d);
  funk_table *t = d ? calloc(1, sizeof *t) : NULL;
  if (!t) {
    free(d);
    last_error = "out of memory";
    return NULL;
  }
  for (int64_t i = 0; i < frames; i++) d[i] = (float)values[i];
  pthread_mutex_lock(&lock);
  for (funk_table *o = tables; o; o = o->next)
    if (!o->path && o->frames == frames && !memcmp(o->data, d, (size_t)frames * sizeof *d)) {
      o->refs++;
      pthread_mutex_unlock(&lock);
      free(d);
      free(t);
      return o;
    }
  t->data = d;
  t->frames = frames;
  t->rate = 1;
  t->refs = 1;
  t->next = tables;
  tables = t;
  pthread_mutex_unlock(&lock);
  return t;
}

void funk_table_release(funk_table *t) {
  if (!t) return;
  pthread_mutex_lock(&lock);
  if (--t->refs) {
    pthread_mutex_unlock(&lock);
    return;
  }
  for (funk_table **q = &tables; *q; q = &(*q)->next)
    if (*q == t) {
      *q = t->next;
      break;
    }
  pthread_mutex_unlock(&lock);
  free(t->data);
  free(t->path);
  free(t);
}

const float *funk_table_data(const funk_table *t) { return t->data; }
int64_t funk_table_frames(const funk_table *t) { return t->frames; }
double funk_table_rate(const funk_table *t) { return t->rate; }
