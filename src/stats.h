#pragma once

/*
 * Per-ID receive statistics, one open-addressed hash table per bus.
 * Each table has exactly one writer (the task that receives on that bus) and
 * any number of lock-free readers.
 */

#include <stdbool.h>
#include <stdint.h>

#include "can_frame.h"

#define STATS_BUSES 2
#define STATS_SLOTS 2048   /* per bus, power of two */
#define STATS_MAX_IDS 1024 /* per bus */

typedef struct {
    uint32_t id;
    uint32_t count;
    uint32_t last_ms;
    bool ext;
    bool rtr;
    uint8_t dlc;
    uint8_t data[8];
} stats_row_t;

void stats_init(void);

/* bus: 0 = CAN1, 1 = CAN2. Call only from that bus's receive task. */
void stats_update(int bus, const can_frame_t *f);

/* Returns false if the slot is empty. */
bool stats_read(int bus, int slot, stats_row_t *out);

/* Number of frames whose ID did not fit in the table. */
uint32_t stats_untracked(int bus);

/* Ask both tables to reset. Takes effect lazily, readers see it at once. */
void stats_clear(void);
uint32_t stats_clear_generation(void);
