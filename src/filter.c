#include "filter.h"

#include <stdatomic.h>
#include <string.h>

#include "can_frame.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_lock;
#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)
#else
/* Host build (unit tests): a single writer is assumed. */
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

typedef struct {
    uint32_t std_bits[FILTER_STD_WORDS];
    uint32_t ext[FILTER_MAX_EXT]; /* sorted ascending */
    uint32_t ext_count;
} filter_set_t;

static filter_set_t s_sets[2];
static _Atomic(const filter_set_t *) s_active = &s_sets[0];
/*
 * Incremented after every swap. A reader that sees the same value before and
 * after its lookup knows the copy it read was not being rewritten meanwhile.
 */
static atomic_uint s_gen;

void filter_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
#endif
}

static int ext_find(const filter_set_t *s, uint32_t n, uint32_t id, bool *found)
{
    int lo = 0, hi = (int)n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        uint32_t v = s->ext[mid];
        if (v == id) {
            *found = true;
            return mid;
        }
        if (v < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = false;
    return lo;
}

bool filter_blocked(uint32_t id, bool ext)
{
    for (;;) {
        unsigned gen = atomic_load_explicit(&s_gen, memory_order_acquire);
        const filter_set_t *s = atomic_load_explicit(&s_active, memory_order_acquire);
        bool blocked;
        if (!ext) {
            id &= CAN_STD_ID_MAX;
            blocked = (s->std_bits[id >> 5] >> (id & 31)) & 1u;
        } else {
            uint32_t n = s->ext_count;
            if (n > FILTER_MAX_EXT) {
                n = FILTER_MAX_EXT;
            }
            ext_find(s, n, id & CAN_EXT_ID_MAX, &blocked);
        }
        if (atomic_load_explicit(&s_gen, memory_order_acquire) == gen) {
            return blocked;
        }
    }
}

/* Writers: call with the lock held. */
static filter_set_t *edit_begin(bool copy)
{
    const filter_set_t *cur = atomic_load(&s_active);
    filter_set_t *next = (cur == &s_sets[0]) ? &s_sets[1] : &s_sets[0];
    if (copy) {
        memcpy(next, cur, sizeof(*next));
    } else {
        memset(next, 0, sizeof(*next));
    }
    return next;
}

static void edit_commit(filter_set_t *next)
{
    atomic_store_explicit(&s_active, next, memory_order_release);
    atomic_fetch_add_explicit(&s_gen, 1, memory_order_release);
}

static filter_err_t set_add(filter_set_t *s, uint32_t id, bool ext)
{
    if (!ext) {
        if (id > CAN_STD_ID_MAX) {
            return FILTER_BAD_ID;
        }
        uint32_t bit = 1u << (id & 31);
        if (s->std_bits[id >> 5] & bit) {
            return FILTER_EXISTS;
        }
        s->std_bits[id >> 5] |= bit;
        return FILTER_OK;
    }
    if (id > CAN_EXT_ID_MAX) {
        return FILTER_BAD_ID;
    }
    bool found;
    int pos = ext_find(s, s->ext_count, id, &found);
    if (found) {
        return FILTER_EXISTS;
    }
    if (s->ext_count >= FILTER_MAX_EXT) {
        return FILTER_FULL;
    }
    memmove(&s->ext[pos + 1], &s->ext[pos], (s->ext_count - pos) * sizeof(uint32_t));
    s->ext[pos] = id;
    s->ext_count++;
    return FILTER_OK;
}

static filter_err_t set_remove(filter_set_t *s, uint32_t id, bool ext)
{
    if (!ext) {
        if (id > CAN_STD_ID_MAX) {
            return FILTER_BAD_ID;
        }
        uint32_t bit = 1u << (id & 31);
        if (!(s->std_bits[id >> 5] & bit)) {
            return FILTER_NOT_FOUND;
        }
        s->std_bits[id >> 5] &= ~bit;
        return FILTER_OK;
    }
    if (id > CAN_EXT_ID_MAX) {
        return FILTER_BAD_ID;
    }
    bool found;
    int pos = ext_find(s, s->ext_count, id, &found);
    if (!found) {
        return FILTER_NOT_FOUND;
    }
    memmove(&s->ext[pos], &s->ext[pos + 1], (s->ext_count - pos - 1) * sizeof(uint32_t));
    s->ext_count--;
    return FILTER_OK;
}

filter_err_t filter_add(uint32_t id, bool ext)
{
    LOCK();
    filter_set_t *next = edit_begin(true);
    filter_err_t err = set_add(next, id, ext);
    if (err == FILTER_OK) {
        edit_commit(next);
    }
    UNLOCK();
    return err;
}

filter_err_t filter_remove(uint32_t id, bool ext)
{
    LOCK();
    filter_set_t *next = edit_begin(true);
    filter_err_t err = set_remove(next, id, ext);
    if (err == FILTER_OK) {
        edit_commit(next);
    }
    UNLOCK();
    return err;
}

void filter_clear(void)
{
    LOCK();
    edit_commit(edit_begin(false));
    UNLOCK();
}

filter_err_t filter_replace(const filter_id_t *ids, size_t n)
{
    filter_err_t result = FILTER_OK;
    LOCK();
    filter_set_t *next = edit_begin(false);
    for (size_t i = 0; i < n; i++) {
        filter_err_t err = set_add(next, ids[i].id, ids[i].ext);
        if (err != FILTER_OK && err != FILTER_EXISTS) {
            result = err;
            break;
        }
    }
    if (result == FILTER_OK) {
        edit_commit(next);
    }
    UNLOCK();
    return result;
}

static size_t std_count(const filter_set_t *s)
{
    size_t n = 0;
    for (int i = 0; i < FILTER_STD_WORDS; i++) {
        n += (size_t)__builtin_popcount(s->std_bits[i]);
    }
    return n;
}

size_t filter_count(void)
{
    LOCK();
    const filter_set_t *s = atomic_load(&s_active);
    size_t n = std_count(s) + s->ext_count;
    UNLOCK();
    return n;
}

size_t filter_list(filter_id_t *out, size_t max)
{
    size_t n = 0;
    LOCK();
    const filter_set_t *s = atomic_load(&s_active);
    for (uint32_t id = 0; id <= CAN_STD_ID_MAX && n < max; id++) {
        if ((s->std_bits[id >> 5] >> (id & 31)) & 1u) {
            out[n].id = id;
            out[n].ext = false;
            n++;
        }
    }
    for (uint32_t i = 0; i < s->ext_count && n < max; i++) {
        out[n].id = s->ext[i];
        out[n].ext = true;
        n++;
    }
    UNLOCK();
    return n;
}

uint32_t filter_generation(void)
{
    return atomic_load(&s_gen);
}

void filter_export_raw(uint32_t std_bits[FILTER_STD_WORDS], uint32_t ext[FILTER_MAX_EXT], uint16_t *ext_count)
{
    LOCK();
    const filter_set_t *s = atomic_load(&s_active);
    memcpy(std_bits, s->std_bits, sizeof(s->std_bits));
    memcpy(ext, s->ext, s->ext_count * sizeof(uint32_t));
    *ext_count = (uint16_t)s->ext_count;
    UNLOCK();
}

void filter_import_raw(const uint32_t std_bits[FILTER_STD_WORDS], const uint32_t *ext, uint16_t ext_count)
{
    LOCK();
    filter_set_t *next = edit_begin(false);
    memcpy(next->std_bits, std_bits, sizeof(next->std_bits));
    for (uint16_t i = 0; i < ext_count; i++) {
        set_add(next, ext[i], true); /* re-sorts and drops anything invalid */
    }
    edit_commit(next);
    UNLOCK();
}

const char *filter_strerror(filter_err_t err)
{
    switch (err) {
    case FILTER_OK: return "ok";
    case FILTER_EXISTS: return "ID is already blocked";
    case FILTER_NOT_FOUND: return "ID is not in the blocklist";
    case FILTER_FULL: return "extended ID list is full (256)";
    case FILTER_BAD_ID: return "ID out of range";
    default: return "unknown error";
    }
}
