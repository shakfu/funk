/* table.c -- read-only sample data for kernels (graph.h). Tables are shared
   across graphs and plugin instances, by file and channel or by content, and
   freed with the last graph that reads them. Host threads only: the audio
   thread sees a pointer that a graph keeps alive. */
#define _DEFAULT_SOURCE
#include "table.h"

#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
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

/* path into out, with a leading "~" or "~/" as $HOME, since a plugin's working
   directory is its host's; 0 if it does not fit */
static int home_path(const char *path, char *out, size_t n) {
  const char *home = getenv("HOME");
  if (path[0] == '~' && (path[1] == '/' || !path[1]) && home && *home)
    return snprintf(out, n, "%s%s", home, path + 1) < (int)n;
  return snprintf(out, n, "%s", path) < (int)n;
}

/* SuperDirt's fileExtensions (DirtSoundLibrary.sc), in any case */
static bool sample_name(const char *name, size_t len) {
  static const char *const exts[] = {".wav", ".aif", ".aiff", ".aifc"};
  for (size_t i = 0; i < sizeof exts / sizeof *exts; i++) {
    size_t k = strlen(exts[i]);
    if (len > k && !strcasecmp(name + len - k, exts[i])) return true;
  }
  return false;
}

const char *funk_sample_path(const char *dir, const char *name, int index) {
  static _Thread_local char out[4096];
  if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
    last_error = "a sample name is one folder's name";
    return "";
  }
  char folder[4096], home[4096];
  if (!home_path(dir, home, sizeof home) || snprintf(folder, sizeof folder, "%s/%s", home, name) >= (int)sizeof folder) {
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
    if (e->d_name[0] == '.' || !sample_name(e->d_name, len)) continue;
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
  if (!n) last_error = "no WAV or AIFF files in the sample folder";
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

static uint32_t u16be(const unsigned char *p) { return (uint32_t)p[0] << 8 | (uint32_t)p[1]; }
static uint32_t u32be(const unsigned char *p) { return u16be(p) << 16 | u16be(p + 2); }

/* An AIFF sample in -1 to 1: kind 0 big-endian integer, 1 little-endian
   (AIFC's sowt), 2 big-endian float. A width under its bytes is left-justified. */
static float aiff_sample(const unsigned char *p, int kind, uint32_t bytes) {
  if (kind == 2) {
    if (bytes == 4) {
      uint32_t u = u32be(p);
      float f;
      memcpy(&f, &u, sizeof f);
      return f;
    }
    uint64_t u = (uint64_t)u32be(p) << 32 | (uint64_t)u32be(p + 4);
    double d;
    memcpy(&d, &u, sizeof d);
    return (float)d;
  }
  uint32_t v = 0;
  for (uint32_t i = 0; i < bytes; i++) v |= (uint32_t)p[kind == 0 ? i : bytes - 1 - i] << (24 - 8 * i);
  return bytes == 4 ? (float)((double)(int32_t)v / 2147483648.0) : (float)((int32_t)v >> (32 - 8 * bytes)) / (float)(1u << (8 * bytes - 1));
}

/* COMM's 80-bit extended sample rate; 0 if out of range */
static double extended(const unsigned char *p) {
  int e = (int)(u16be(p) & 0x7fff) - 16383 - 63;
  uint64_t m = (uint64_t)u32be(p + 2) << 32 | u32be(p + 6);
  if (!m || e > 0 || e < -1100) return 0;
  double d = (double)m;
  for (; e < 0; e++) d *= 0.5;
  return d;
}

/* AIFF and AIFC (as libsndfile, which scsynth reads samples with): FORM, a
   COMM chunk and an SSND chunk. Untrusted as decode's. */
static const char *decode_aiff(const unsigned char *b, size_t n, int channel, funk_table *t) {
  bool aifc = !memcmp(b + 8, "AIFC", 4);
  if (!aifc && memcmp(b + 8, "AIFF", 4)) return "not an AIFF file";
  const unsigned char *comm = NULL, *ssnd = NULL;
  size_t comm_n = 0, ssnd_n = 0;
  for (size_t at = 12; n - at >= 8;) {
    size_t len = u32be(b + at + 4), body = at + 8, left = n - body;
    if (!memcmp(b + at, "COMM", 4) && len <= left) {
      comm = b + body;
      comm_n = len;
    } else if (!memcmp(b + at, "SSND", 4)) {
      ssnd = b + body;
      ssnd_n = len < left ? len : left;
    }
    if (len >= left) break;
    at = body + len + (len & 1);
    if (at > n) break;
  }
  if (!comm || comm_n < (aifc ? 22u : 18u)) return "no COMM chunk";
  if (!ssnd || ssnd_n < 8) return "no SSND chunk";
  uint32_t channels = u16be(comm), bits = u16be(comm + 6);
  double rate = extended(comm + 8);
  int kind = 0;
  if (aifc) {
    const unsigned char *c = comm + 18;
    if (!memcmp(c, "sowt", 4)) kind = 1;
    else if (!memcmp(c, "fl32", 4) || !memcmp(c, "FL32", 4) || !memcmp(c, "fl64", 4) || !memcmp(c, "FL64", 4)) kind = 2;
    else if (memcmp(c, "NONE", 4) && memcmp(c, "twos", 4)) return "unsupported AIFC compression";
    if (kind == 2) bits = c[2] == '3' ? 32 : 64;
  }
  uint32_t bytes = (bits + 7) / 8;
  if (!channels || rate <= 0 || !bits) return "bad format";
  if (kind == 2 ? bytes != 4 && bytes != 8 : bytes > 4) return "unsupported sample size";
  size_t offset = u32be(ssnd);
  if (offset > ssnd_n - 8) return "bad SSND offset";
  const unsigned char *data = ssnd + 8 + offset;
  size_t align = (size_t)channels * bytes, frames = (ssnd_n - 8 - offset) / align;
  if (frames > INT32_MAX) return "too long";
  int c = channel < (int)channels ? channel : (int)channels - 1;
  t->data = malloc((frames ? frames : 1) * sizeof(float));
  if (!t->data) return "out of memory";
  for (size_t i = 0; i < frames; i++) t->data[i] = aiff_sample(data + i * align + (size_t)c * bytes, kind, bytes);
  t->frames = (int64_t)frames;
  t->rate = rate;
  return NULL;
}

/* The file's bytes are untrusted: every offset is checked against n. */
static const char *decode(const unsigned char *b, size_t n, int channel, funk_table *t) {
  if (n >= 12 && !memcmp(b, "FORM", 4)) return decode_aiff(b, n, channel, t);
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

funk_table *funk_table_file(const char *given, int channel) {
  struct stat st;
  char path[4096];
  if (!given || channel < 0) {
    last_error = "bad argument";
    return NULL;
  }
  if (!home_path(given, path, sizeof path)) {
    last_error = "the path is too long";
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

extern const unsigned char funk_mdapiano[]; /* mdapiano.c, from thirdparty/mdapiano by embed.cmake */
extern const size_t funk_mdapiano_size;

funk_table *funk_table_builtin(const char *name, int channel) {
  if (!name || channel < 0 || strcmp(name, "mdapiano")) {
    last_error = "no such built-in table";
    return NULL;
  }
  pthread_mutex_lock(&lock);
  for (funk_table *t = tables; t; t = t->next)
    if (t->path && t->size < 0 && t->channel == channel && !strcmp(t->path, name)) {
      t->refs++;
      pthread_mutex_unlock(&lock);
      return t;
    }
  pthread_mutex_unlock(&lock);
  funk_table *t = calloc(1, sizeof *t);
  char *copy = malloc(strlen(name) + 1);
  const char *why = !t || !copy ? "out of memory" : decode(funk_mdapiano, funk_mdapiano_size, channel, t);
  if (why) {
    if (t) free(t->data);
    free(t);
    free(copy);
    last_error = why;
    return NULL;
  }
  t->path = strcpy(copy, name);
  t->channel = channel;
  t->size = -1; /* never a file's */
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
