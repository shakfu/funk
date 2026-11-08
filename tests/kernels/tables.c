/* projects/funkgraph/table.c and lib/kernels.fk's readers. The WAV reader takes
   every format and refuses malformed files without reading outside them;
   tables are shared and follow a replaced file; the sampler and wavetable
   read them; and an audio thread renders while this thread replaces the file
   and plays 400 graphs. Run under AddressSanitizer and ThreadSanitizer too. */
#define _DEFAULT_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "graph.h"
#include "table.h"

static char path[512];

static void put16(FILE *f, unsigned v) { fputc((int)(v & 255), f), fputc((int)(v >> 8 & 255), f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xffff), put16(f, v >> 16); }

/* A WAV of format, channels, bits, rate and n data bytes; extra adds a chunk first. */
static FILE *wav(unsigned format, unsigned channels, unsigned bits, unsigned rate, unsigned n, bool extra) {
  FILE *f = fopen(path, "wb");
  if (!f) exit(1);
  fwrite("RIFF", 1, 4, f), put32(f, 36 + n), fwrite("WAVE", 1, 4, f);
  if (extra) fwrite("LIST", 1, 4, f), put32(f, 3), fwrite("abc", 1, 4, f); /* odd: a pad byte */
  fwrite("fmt ", 1, 4, f), put32(f, format == 0xFFFE ? 40 : 16), put16(f, format), put16(f, channels), put32(f, rate);
  put32(f, rate * channels * bits / 8), put16(f, channels * bits / 8), put16(f, bits);
  if (format == 0xFFFE) put16(f, 22), put16(f, bits), put32(f, 3), put16(f, 3), fwrite("0123456789abcd", 1, 14, f);
  fwrite("data", 1, 4, f), put32(f, n);
  return f;
}

static void show(const char *what, int channel) {
  funk_table *t = funk_table_file(path, channel);
  if (!t) {
    printf("%s: %s\n", what, funk_table_error());
    return;
  }
  printf("%s: %lld frames at %.0f:", what, (long long)funk_table_frames(t), funk_table_rate(t));
  for (int64_t i = 0; i < funk_table_frames(t) && i < 4; i++) printf(" %.4f", funk_table_data(t)[i]);
  printf("\n");
  funk_table_release(t);
}

static void bytes(const char *what, const void *b, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) exit(1);
  fwrite(b, 1, n, f);
  fclose(f);
  show(what, 0);
}

static void formats(void) {
  FILE *f = wav(1, 1, 8, 8000, 2, false);
  fputc(192, f), fputc(64, f), fclose(f);
  show("u8", 0);
  f = wav(1, 1, 16, 8000, 4, true);
  put16(f, 16384), put16(f, 0xC000), fclose(f);
  show("s16, after another chunk", 0);
  f = wav(1, 1, 24, 44100, 3, false);
  fputc(0, f), fputc(0, f), fputc(0xC0, f), fclose(f);
  show("s24", 0);
  f = wav(1, 1, 32, 8000, 4, false);
  put32(f, 0x40000000), fclose(f);
  show("s32", 0);
  float x = 0.25f;
  double y = 0.125;
  f = wav(3, 1, 32, 8000, 4, false);
  fwrite(&x, 4, 1, f), fclose(f);
  show("f32", 0);
  f = wav(3, 1, 64, 8000, 8, false);
  fwrite(&y, 8, 1, f), fclose(f);
  show("f64", 0);
  f = wav(0xFFFE, 1, 32, 8000, 4, false);
  fwrite(&x, 4, 1, f), fclose(f);
  show("extensible f32", 0);
  f = wav(1, 2, 16, 8000, 8, false);
  put16(f, 8192), put16(f, 16384), put16(f, 24576), put16(f, 32767), fclose(f);
  show("stereo, left", 0);
  show("stereo, right", 1);
  show("stereo, a channel it lacks", 7);
  f = wav(1, 1, 16, 8000, 1000, false); /* a header promising more than the file holds */
  put16(f, 16384), fclose(f);
  show("data cut short", 0);
  f = wav(1, 1, 16, 8000, 0, false);
  fclose(f);
  show("no frames", 0);
}

static void malformed(void) {
  bytes("empty", "", 0);
  bytes("not RIFF", "RIFX....WAVE", 12);
  bytes("only a header", "RIFF....WAVE", 12);
  FILE *f = wav(2, 1, 16, 8000, 2, false);
  put16(f, 0), fclose(f);
  show("ADPCM", 0);
  f = wav(1, 0, 16, 8000, 2, false);
  put16(f, 0), fclose(f);
  show("no channels", 0);
  f = wav(1, 1, 12, 8000, 2, false);
  put16(f, 0), fclose(f);
  show("12 bits", 0);
  f = wav(3, 1, 16, 8000, 2, false);
  put16(f, 0), fclose(f);
  show("f16", 0);
  f = wav(1, 1, 16, 0, 2, false);
  put16(f, 0), fclose(f);
  show("rate 0", 0);
  static const unsigned char huge[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 0xff, 0xff, 0xff, 0xff, 1};
  bytes("a chunk larger than the file", huge, sizeof huge);
  static const unsigned char small[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 4, 0, 0, 0, 1, 0, 1, 0};
  bytes("a fmt chunk too small", small, sizeof small);
  static const unsigned char align[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 64, 31, 0, 0, 0, 0, 0, 0, 7, 0, 16, 0, 'd', 'a', 't', 'a', 2, 0, 0, 0, 0, 0};
  bytes("a frame size that disagrees", align, sizeof align);
  unlink(path);
  show("missing", 0);
  char dir[520];
  strcpy(dir, path);
  char *slash = strrchr(dir, '/');
  if (slash) *slash = 0;
  printf("a directory: %s\n", funk_table_file(slash ? dir : ".", 0) ? "loaded" : funk_table_error());
}

/* a file of frames copies of value */
static void constant(float value, int frames, unsigned rate) {
  char tmp[530];
  snprintf(tmp, sizeof tmp, "%s.new", path);
  char saved[512];
  strcpy(saved, path);
  strcpy(path, tmp);
  FILE *f = wav(3, 1, 32, rate, (unsigned)frames * 4, false);
  for (int i = 0; i < frames; i++) fwrite(&value, 4, 1, f);
  fclose(f);
  strcpy(path, saved);
  rename(tmp, path); /* as an editor replaces a file */
}

static void sharing(void) {
  constant(0.5f, 16, 8000);
  funk_table *a = funk_table_file(path, 0), *b = funk_table_file(path, 0), *c = funk_table_file(path, 1);
  printf("one file and channel is shared: %d; another channel is not: %d\n", a == b, a != c);
  usleep(20000); /* a later modification time on any file system with one */
  constant(0.25f, 16, 8000);
  funk_table *d = funk_table_file(path, 0);
  printf("a replaced file is read again: %d, the old stays readable: %.2f, the new: %.2f\n", d != a, funk_table_data(a)[0], funk_table_data(d)[0]);
  funk_table_release(a), funk_table_release(b), funk_table_release(c), funk_table_release(d);
  double v[3] = {1, 2, 3}, w[3] = {1, 2, 4};
  a = funk_table_values(v, 3), b = funk_table_values(v, 3), c = funk_table_values(w, 3);
  printf("equal values are shared: %d; others are not: %d\n", a == b, a != c);
  funk_table_release(a), funk_table_release(b), funk_table_release(c);
  printf("a negative length: %s\n", funk_table_values(v, -1) ? "accepted" : funk_table_error());
}

static funk_kernel sampler, wavetable;

static funk_graph *play_graph(float gate, float speed, const char *name) {
  funk_graph *g = funk_graph_new(8, 8000.0);
  int gt = funk_graph_control(g, gate), sp = funk_graph_control(g, speed);
  funk_graph_name(g, gt, "gate");
  int in[2] = {gt, sp};
  double st[5] = {0, 0, 0, 1e30, 0};
  int n = funk_graph_add(g, sampler, st, sizeof st, in, 2);
  if (funk_graph_table_file(g, n, path, 0) < 0) {
    printf("table: %s\n", funk_table_error());
    exit(1);
  }
  if (name) funk_graph_name(g, n, name);
  return g;
}

static void row(const char *what, funk_player *p, int n) {
  float out[32];
  funk_player_render(p, out, n);
  printf("%s:", what);
  for (int i = 0; i < n; i++) printf(" %.3f", out[i]);
  printf("\n");
}

static void kernels(void) {
  float ramp[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  FILE *f = wav(3, 1, 32, 4000, 16, false);
  fwrite(ramp, 4, 4, f), fclose(f);
  funk_player *p = funk_player_new();
  funk_player_play(p, play_graph(0, 1, "s"));
  row("silent before a gate", p, 4);
  funk_player_send(p, funk_player_frames(p) + 1, "gate", 1);
  row("at half the graph's rate, once", p, 12);
  funk_player_send(p, funk_player_frames(p), "gate", 0);
  row("a falling gate does not stop it, and it stays ended", p, 2);
  funk_player_send(p, funk_player_frames(p), "gate", 1);
  row("a rising gate restarts it", p, 3);
  /* a replaced file: the node keeps its position, and reads the new data */
  constant(0.75f, 64, 8000);
  funk_player_play(p, play_graph(1, 1, "s"));
  row("the new file, from where it was", p, 2);
  funk_player_free(p);

  p = funk_player_new();
  funk_graph *g = funk_graph_new(8, 8000.0);
  int fr = funk_graph_control(g, 2000);
  double st[5] = {0}, v[4] = {0, 1, 0, -1};
  int n = funk_graph_add(g, wavetable, st, sizeof st, &fr, 1);
  funk_graph_table_values(g, n, v, 4);
  funk_player_play(p, g);
  row("a wavetable, one frame per entry", p, 6);
  funk_player_free(p);

  p = funk_player_new();
  g = funk_graph_new(8, 8000.0);
  fr = funk_graph_control(g, 1000);
  n = funk_graph_add(g, wavetable, st, sizeof st, &fr, 1);
  funk_graph_table_values(g, n, v, 4);
  funk_player_play(p, g);
  row("between entries, wrapping", p, 9);
  funk_player_free(p);

  /* odd inputs stay inside the data */
  p = funk_player_new();
  g = funk_graph_new(8, 8000.0);
  fr = funk_graph_control(g, 0.0f / 0.0f);
  n = funk_graph_add(g, wavetable, st, sizeof st, &fr, 1);
  funk_graph_table_values(g, n, v, 4);
  funk_player_play(p, g);
  float junk[8];
  funk_player_render(p, junk, 8);
  printf("a NaN frequency stays in bounds\n");
  funk_player_free(p);
  p = funk_player_new();
  g = funk_graph_new(8, 8000.0);
  fr = funk_graph_control(g, 100);
  n = funk_graph_add(g, wavetable, st, sizeof st, &fr, 1);
  funk_graph_table_values(g, n, NULL, 0);
  funk_player_play(p, g);
  row("an empty wavetable is silent", p, 2);
  funk_player_free(p);

  funk_graph *h = funk_graph_new(8, 8000.0);
  double little[2] = {0, 0};
  fr = funk_graph_control(h, 1);
  n = funk_graph_add(h, wavetable, little, sizeof little, &fr, 1);
  printf("refused: state too small %d, a control %d, a second table:", funk_graph_table_values(h, n, v, 4), funk_graph_table_values(h, fr, v, 4));
  n = funk_graph_add(h, wavetable, st, sizeof st, &fr, 1);
  funk_graph_table_values(h, n, v, 4);
  printf(" %d\n", funk_graph_table_values(h, n, v, 4));
  funk_graph_free(h);
}

static funk_player *player;
static atomic_int stop, blocks, bad;

static void *audio(void *arg) {
  (void)arg;
  while (!atomic_load(&stop)) {
    float out[64];
    funk_player_render(player, out, 64);
    for (int i = 0; i < 64; i++) /* always one file or the other's value, or the sample's end */
      if (out[i] != 0 && out[i] != 0.5f && out[i] != 0.25f) atomic_fetch_add(&bad, 1);
    atomic_fetch_add(&blocks, 1);
    funk_player_send(player, funk_player_frames(player), "gate", 0);
    funk_player_render(player, out, 8);
    funk_player_send(player, funk_player_frames(player), "gate", 1);
  }
  return NULL;
}

/* A sample of 2000 frames, about 30 blocks. */
static void swaps(void) {
  player = funk_player_new();
  constant(0.5f, 2000, 8000);
  funk_player_play(player, play_graph(1, 1, "s"));
  pthread_t t;
  pthread_create(&t, NULL, audio, NULL);
  for (int i = 0; i < 400; i++) {
    constant(i % 2 ? 0.5f : 0.25f, 2000, 8000);
    funk_player_play(player, play_graph(1, 1, "s"));
    while (atomic_load(&blocks) < i) usleep(10);
  }
  atomic_store(&stop, 1);
  pthread_join(t, NULL);
  funk_player_free(player);
  printf("400 replaced files during playback: %d wrong samples\n", atomic_load(&bad));
}

/* funk_sample_path: dir/name's WAV files, sorted, the index wrapping; a
   name is one folder, never a path out of dir */
static void touch(const char *p) {
  FILE *f = fopen(p, "w");
  if (f) fclose(f);
}

static void sample_paths(void) {
  char dir[600], bd[700], empty[700], f[800];
  snprintf(dir, sizeof dir, "%s.kit", path);
  snprintf(bd, sizeof bd, "%s/bd", dir);
  snprintf(empty, sizeof empty, "%s/empty", dir);
  mkdir(dir, 0700), mkdir(bd, 0700), mkdir(empty, 0700);
  const char *files[] = {"b.wav", "a.WAV", "c.txt", ".d.wav"};
  for (int i = 0; i < 4; i++) snprintf(f, sizeof f, "%s/%s", bd, files[i]), touch(f);
  printf("sample paths:");
  for (int i = -1; i <= 2; i++) {
    const char *p = funk_sample_path(dir, "bd", i);
    printf(" %d %s", i, *p ? strrchr(p, '/') + 1 : "-");
  }
  printf("\n");
  const char *bad[] = {"sn", "empty", "../bd", "bd/a.WAV", "", ".."};
  for (int i = 0; i < 6; i++) {
    const char *p = funk_sample_path(dir, bad[i], 0);
    printf("sample path '%s': '%s', %s\n", bad[i], p, funk_table_error());
  }
  /* "~" and "~/" are $HOME, for a plugin, whose working directory is its host's;
     "~x" is not */
  const char *home = getenv("HOME");
  char saved[4096];
  snprintf(saved, sizeof saved, "%s", home ? home : "");
  setenv("HOME", dir, 1);
  const char *homes[] = {"~", "~/", "~x"};
  for (int i = 0; i < 3; i++) {
    const char *p = funk_sample_path(homes[i], "bd", 0);
    printf("sample path in '%s': %s\n", homes[i], *p ? strrchr(p, '/') + 1 : funk_table_error());
  }
  snprintf(f, sizeof f, "%s/bd/b.wav", dir);
  FILE *w = wav(1, 1, 16, 44100, 2, false);
  put16(w, 0), fclose(w), rename(path, f);
  funk_table *t = funk_table_file("~/bd/b.wav", 0);
  printf("a table from '~/bd/b.wav': %s\n", t ? "loaded" : funk_table_error());
  if (t) funk_table_release(t);
  if (home) setenv("HOME", saved, 1);
  else unsetenv("HOME");
  for (int i = 0; i < 4; i++) snprintf(f, sizeof f, "%s/%s", bd, files[i]), unlink(f);
  rmdir(bd), rmdir(empty), rmdir(dir);
}

int main(int argc, char **argv) {
  void *lib = argc > 1 ? dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) : NULL;
  if (!lib) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  sampler = (funk_kernel)dlsym(lib, "dsp_sampler");
  wavetable = (funk_kernel)dlsym(lib, "dsp_wavetable");
  if (!sampler || !wavetable) return 1;
  snprintf(path, sizeof path, "%s.%d.wav", argv[1], (int)getpid());
  formats();
  malformed();
  sharing();
  kernels();
  swaps();
  sample_paths();
  unlink(path);
  return 0;
}
