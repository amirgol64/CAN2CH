#pragma once

/*
 * Blocklist filter.
 *
 * Standard IDs live in a 2048-bit bitmap, extended IDs in a sorted array.
 * filter_blocked() never takes a lock: edits are made on an inactive copy
 * which is then swapped in atomically.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FILTER_STD_WORDS 64
#define FILTER_MAX_EXT 256
#define FILTER_MAX_TOTAL (2048 + FILTER_MAX_EXT)

typedef struct {
    uint32_t id;
    bool ext;
} filter_id_t;

typedef enum {
    FILTER_OK = 0,
    FILTER_EXISTS,
    FILTER_NOT_FOUND,
    FILTER_FULL,
    FILTER_BAD_ID,
} filter_err_t;

void filter_init(void);

/* Real-time path. Safe to call concurrently with any of the edit functions. */
bool filter_blocked(uint32_t id, bool ext);

filter_err_t filter_add(uint32_t id, bool ext);
filter_err_t filter_remove(uint32_t id, bool ext);
void filter_clear(void);
/* Replace the whole list in one swap. Duplicates are ignored. */
filter_err_t filter_replace(const filter_id_t *ids, size_t n);

size_t filter_count(void);
/* Standard IDs first, each group ascending. Returns the number written. */
size_t filter_list(filter_id_t *out, size_t max);

/* Incremented on every change, so clients can tell when to refetch. */
uint32_t filter_generation(void);

/* Raw access for persistence. */
void filter_export_raw(uint32_t std_bits[FILTER_STD_WORDS], uint32_t ext[FILTER_MAX_EXT], uint16_t *ext_count);
void filter_import_raw(const uint32_t std_bits[FILTER_STD_WORDS], const uint32_t *ext, uint16_t ext_count);

const char *filter_strerror(filter_err_t err);
