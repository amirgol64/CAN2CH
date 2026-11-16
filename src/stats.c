#include "stats.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#define KEY_USED 0x80000000u
#define KEY_EXT 0x40000000u

typedef struct {
    atomic_uint seq; /* odd while the writer is updating the entry */
    uint32_t key;
    uint32_t count;
    uint32_t last_ms;
    uint8_t dlc;
    uint8_t flags;
    uint8_t data[8];
} entry_t;

static entry_t *s_table[STATS_BUSES];
static uint32_t s_used[STATS_BUSES];
static atomic_uint s_untracked[STATS_BUSES];
static atomic_bool s_clear_req[STATS_BUSES];
static atomic_uint s_clear_gen;

void stats_init(void)
{
    for (int b = 0; b < STATS_BUSES; b++) {
        s_table[b] = heap_caps_calloc_prefer(STATS_SLOTS, sizeof(entry_t), 2,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                             MALLOC_CAP_DEFAULT);
        assert(s_table[b]);
    }
}

static inline uint32_t hash(uint32_t key)
{
    return (key * 2654435761u) >> 21; /* 11 bits */
}

void stats_update(int bus, const can_frame_t *f)
{
    entry_t *t = s_table[bus];
    if (atomic_load_explicit(&s_clear_req[bus], memory_order_relaxed)) {
        memset(t, 0, STATS_SLOTS * sizeof(entry_t));
        s_used[bus] = 0;
        atomic_store(&s_untracked[bus], 0);
        atomic_store(&s_clear_req[bus], false);
    }

    uint32_t key = KEY_USED | f->id | ((f->flags & CAN_FLAG_EXT) ? KEY_EXT : 0);
    uint32_t i = hash(key) & (STATS_SLOTS - 1);
    entry_t *e;
    for (;;) {
        e = &t[i];
        if (e->key == key) {
            break;
        }
        if (e->key == 0) {
            if (s_used[bus] >= STATS_MAX_IDS) {
                atomic_fetch_add_explicit(&s_untracked[bus], 1, memory_order_relaxed);
                return;
            }
            s_used[bus]++;
            break;
        }
        i = (i + 1) & (STATS_SLOTS - 1);
    }

    unsigned seq = atomic_load_explicit(&e->seq, memory_order_relaxed);
    atomic_store_explicit(&e->seq, seq + 1, memory_order_release);
    e->count++;
    e->last_ms = (uint32_t)(esp_timer_get_time() / 1000);
    e->dlc = f->dlc;
    e->flags = f->flags;
    memcpy(e->data, f->data, 8);
    e->key = key;
    atomic_store_explicit(&e->seq, seq + 2, memory_order_release);
}

bool stats_read(int bus, int slot, stats_row_t *out)
{
    if (atomic_load(&s_clear_req[bus])) {
        return false;
    }
    const entry_t *e = &s_table[bus][slot];
    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned seq = atomic_load_explicit(&((entry_t *)e)->seq, memory_order_acquire);
        if (seq & 1u) {
            continue;
        }
        uint32_t key = e->key;
        out->count = e->count;
        out->last_ms = e->last_ms;
        out->dlc = e->dlc;
        out->rtr = (e->flags & CAN_FLAG_RTR) != 0;
        memcpy(out->data, e->data, 8);
        if (atomic_load_explicit(&((entry_t *)e)->seq, memory_order_acquire) != seq) {
            continue;
        }
        if (!(key & KEY_USED)) {
            return false;
        }
        out->ext = (key & KEY_EXT) != 0;
        out->id = key & CAN_EXT_ID_MAX;
        return true;
    }
    return false;
}

uint32_t stats_untracked(int bus)
{
    return atomic_load(&s_untracked[bus]);
}

void stats_clear(void)
{
    for (int b = 0; b < STATS_BUSES; b++) {
        atomic_store(&s_clear_req[b], true);
    }
    atomic_fetch_add(&s_clear_gen, 1);
}

uint32_t stats_clear_generation(void)
{
    return atomic_load(&s_clear_gen);
}
