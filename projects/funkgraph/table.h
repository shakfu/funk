/* table.h -- read-only sample data for kernels (table.c). Host threads only. */
#ifndef FUNK_TABLE_H
#define FUNK_TABLE_H

#include <stdint.h>

typedef struct funk_table funk_table;

/* Channel c of a WAV file (integer of 8 to 32 bits, or float of 32 or 64), or
   its last channel if it has fewer. An unchanged file already loaded is
   shared; a replaced one is read again. NULL on failure: funk_table_error. */
funk_table *funk_table_file(const char *path, int channel);

/* A copy of values as floats; equal tables are shared. */
funk_table *funk_table_values(const double *values, int64_t frames);

/* Why this thread's last call returned NULL. */
const char *funk_table_error(void);

void funk_table_release(funk_table *t);

const float *funk_table_data(const funk_table *t);
int64_t funk_table_frames(const funk_table *t);
double funk_table_rate(const funk_table *t); /* frames per second; 1 for values */

#endif
