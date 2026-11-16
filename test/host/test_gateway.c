/*
 * Host unit tests for the portable parts of the firmware:
 * filter, ring buffer, XL2515 bit timing and config JSON.
 * Build and run with:  python test/run_tests.py
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can_frame.h"
#include "config.h"
#include "filter.h"
#include "ring.h"
#include "xl2515.h"

#ifdef _WIN32
#include <windows.h>
typedef HANDLE thread_t;
#define THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
#define THREAD_RETURN return 0
static void thread_start(thread_t *t, LPTHREAD_START_ROUTINE fn) { *t = CreateThread(NULL, 0, fn, NULL, 0, NULL); }
static void thread_join(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
#include <pthread.h>
typedef pthread_t thread_t;
#define THREAD_FN(name) static void *name(void *arg)
#define THREAD_RETURN return NULL
static void thread_start(thread_t *t, void *(*fn)(void *)) { pthread_create(t, NULL, fn, NULL); }
static void thread_join(thread_t t) { pthread_join(t, NULL); }
#endif

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                    \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failures++;                                              \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------ filter -- */

static void test_filter_basic(void)
{
    filter_init();
    filter_clear();
    CHECK(filter_count() == 0);
    CHECK(!filter_blocked(0x182, false));

    CHECK(filter_add(0x182, false) == FILTER_OK);
    CHECK(filter_add(0x427, false) == FILTER_OK);
    CHECK(filter_add(0x182, false) == FILTER_EXISTS);
    CHECK(filter_add(0x800, false) == FILTER_BAD_ID);
    CHECK(filter_blocked(0x182, false));
    CHECK(filter_blocked(0x427, false));
    CHECK(!filter_blocked(0x183, false));
    /* A standard and an extended frame with the same number are different IDs. */
    CHECK(!filter_blocked(0x182, true));

    CHECK(filter_add(0x000, false) == FILTER_OK);
    CHECK(filter_add(0x7FF, false) == FILTER_OK);
    CHECK(filter_blocked(0x000, false) && filter_blocked(0x7FF, false));
    CHECK(filter_count() == 4);

    CHECK(filter_remove(0x427, false) == FILTER_OK);
    CHECK(filter_remove(0x427, false) == FILTER_NOT_FOUND);
    CHECK(!filter_blocked(0x427, false));
    CHECK(filter_count() == 3);
}

static void test_filter_extended(void)
{
    filter_clear();
    CHECK(filter_add(0x20000000, true) == FILTER_BAD_ID);

    /* Insert out of order; the list must stay sorted and searchable. */
    static const uint32_t ids[] = {0x18FF50E5, 0x00000001, 0x1FFFFFFF, 0x0CF00400, 0x00000182, 0x10000000};
    const size_t n = sizeof(ids) / sizeof(ids[0]);
    for (size_t i = 0; i < n; i++) {
        CHECK(filter_add(ids[i], true) == FILTER_OK);
    }
    CHECK(filter_add(0x0CF00400, true) == FILTER_EXISTS);
    for (size_t i = 0; i < n; i++) {
        CHECK(filter_blocked(ids[i], true));
        CHECK(!filter_blocked(ids[i] ^ 0x40, true));
    }
    CHECK(!filter_blocked(0x182, false));

    filter_id_t list[16];
    size_t count = filter_list(list, 16);
    CHECK(count == n);
    for (size_t i = 1; i < count; i++) {
        CHECK(list[i].ext && list[i - 1].id < list[i].id);
    }

    CHECK(filter_remove(0x00000001, true) == FILTER_OK);
    CHECK(filter_remove(0x1FFFFFFF, true) == FILTER_OK);
    CHECK(!filter_blocked(0x00000001, true) && !filter_blocked(0x1FFFFFFF, true));
    CHECK(filter_blocked(0x18FF50E5, true));
    CHECK(filter_count() == n - 2);

    /* Capacity. */
    filter_clear();
    for (uint32_t i = 0; i < FILTER_MAX_EXT; i++) {
        CHECK(filter_add(0x1000 + i * 3, true) == FILTER_OK);
    }
    CHECK(filter_add(0x5, true) == FILTER_FULL);
    CHECK(filter_blocked(0x1000 + 255 * 3, true));
    CHECK(!filter_blocked(0x5, true));
}

static void test_filter_replace_and_raw(void)
{
    static const filter_id_t ids[] = {{0x100, false}, {0x18FF50E5, true}, {0x100, false}, {0x7FF, false}};
    CHECK(filter_replace(ids, 4) == FILTER_OK);
    CHECK(filter_count() == 3);
    CHECK(filter_blocked(0x100, false) && filter_blocked(0x7FF, false) && filter_blocked(0x18FF50E5, true));

    /* An invalid entry leaves the old list in place. */
    static const filter_id_t bad[] = {{0x200, false}, {0x900, false}};
    uint32_t gen = filter_generation();
    CHECK(filter_replace(bad, 2) == FILTER_BAD_ID);
    CHECK(filter_generation() == gen);
    CHECK(!filter_blocked(0x200, false) && filter_blocked(0x100, false));

    uint32_t std_bits[FILTER_STD_WORDS];
    static uint32_t ext[FILTER_MAX_EXT];
    uint16_t ext_count;
    filter_export_raw(std_bits, ext, &ext_count);
    CHECK(ext_count == 1);
    filter_clear();
    CHECK(filter_count() == 0);
    filter_import_raw(std_bits, ext, ext_count);
    CHECK(filter_count() == 3);
    CHECK(filter_blocked(0x100, false) && filter_blocked(0x18FF50E5, true));
}

/* Lookups must stay correct while another thread keeps editing the list. */
static atomic_bool s_stop;
static atomic_uint s_reader_errors;
static atomic_uint s_reader_loops;

THREAD_FN(filter_reader)
{
    (void)arg;
    unsigned errors = 0, loops = 0;
    while (!atomic_load(&s_stop)) {
        /* These never change while the writer runs. */
        if (!filter_blocked(0x182, false)) errors++;
        if (!filter_blocked(0x18FF50E5, true)) errors++;
        if (!filter_blocked(0x00000010, true)) errors++;
        if (!filter_blocked(0x1FFFFFF0, true)) errors++;
        if (filter_blocked(0x183, false)) errors++;
        if (filter_blocked(0x18FF50E6, true)) errors++;
        loops++;
    }
    atomic_store(&s_reader_errors, errors);
    atomic_store(&s_reader_loops, loops);
    THREAD_RETURN;
}

static void test_filter_concurrent(void)
{
    filter_clear();
    filter_add(0x182, false);
    filter_add(0x18FF50E5, true);
    filter_add(0x00000010, true);
    filter_add(0x1FFFFFF0, true);

    atomic_store(&s_stop, false);
    thread_t reader;
    thread_start(&reader, filter_reader);

    /* Churn entries below, between and above the fixed ones, so the sorted
     * array keeps shifting under the reader. */
    for (int round = 0; round < 20000; round++) {
        for (uint32_t k = 0; k < 8; k++) {
            filter_add(0x00000001 + k, true);
            filter_add(0x10000000 + k, true);
            filter_add(0x1FFFFFF8 + k, true);
            filter_add(0x300 + k, false);
        }
        for (uint32_t k = 0; k < 8; k++) {
            filter_remove(0x00000001 + k, true);
            filter_remove(0x10000000 + k, true);
            filter_remove(0x1FFFFFF8 + k, true);
            filter_remove(0x300 + k, false);
        }
    }
    atomic_store(&s_stop, true);
    thread_join(reader);
    printf("  concurrent lookups: %u rounds, %u wrong answers\n",
           atomic_load(&s_reader_loops), atomic_load(&s_reader_errors));
    CHECK(atomic_load(&s_reader_loops) > 0);
    CHECK(atomic_load(&s_reader_errors) == 0);
    CHECK(filter_count() == 4);
}

/* -------------------------------------------------------------- ring -- */

static ring_t s_ring;
#define RING_TEST_FRAMES 2000000u
static atomic_uint s_ring_errors;

THREAD_FN(ring_consumer)
{
    (void)arg;
    unsigned errors = 0;
    can_frame_t f;
    for (uint32_t expect = 0; expect < RING_TEST_FRAMES;) {
        if (!ring_pop(&s_ring, &f)) {
            continue;
        }
        uint32_t payload;
        memcpy(&payload, f.data, 4);
        if (f.id != expect || payload != ~expect) {
            errors++;
        }
        expect++;
    }
    atomic_store(&s_ring_errors, errors);
    THREAD_RETURN;
}

static void test_ring(void)
{
    can_frame_t f = {0}, out;
    CHECK(ring_count(&s_ring) == 0);
    CHECK(!ring_pop(&s_ring, &out));

    for (uint32_t i = 0; i < RING_SIZE; i++) {
        f.id = i;
        CHECK(ring_push(&s_ring, &f));
    }
    CHECK(ring_count(&s_ring) == RING_SIZE);
    CHECK(!ring_push(&s_ring, &f)); /* full: refuse, never overwrite */
    CHECK(ring_pop(&s_ring, &out) && out.id == 0);
    CHECK(ring_push(&s_ring, &f));
    CHECK(ring_flush(&s_ring) == RING_SIZE);
    CHECK(ring_count(&s_ring) == 0);
    CHECK(!ring_pop(&s_ring, &out));

    /* One producer, one consumer, on different threads. */
    thread_t consumer;
    thread_start(&consumer, ring_consumer);
    for (uint32_t i = 0; i < RING_TEST_FRAMES;) {
        uint32_t payload = ~i;
        f.id = i;
        memcpy(f.data, &payload, 4);
        if (ring_push(&s_ring, &f)) {
            i++;
        }
    }
    thread_join(consumer);
    CHECK(atomic_load(&s_ring_errors) == 0);
    CHECK(ring_count(&s_ring) == 0);
}

/* ------------------------------------------------------------ timing -- */

static void test_timing(void)
{
    static const uint32_t rates[] = {25, 50, 100, 125, 250, 500, 800, 1000};
    static const uint32_t crystals[] = {8000000, 16000000, 20000000};
    for (size_t c = 0; c < 3; c++) {
        for (size_t r = 0; r < 8; r++) {
            uint32_t bps = rates[r] * 1000;
            xl2515_timing_t t;
            bool ok = xl2515_calc_timing(crystals[c], bps, &t);
            if (crystals[c] == 20000000 && rates[r] == 800) {
                CHECK(!ok); /* 12.5 time quanta: not possible */
                continue;
            }
            CHECK(ok);
            if (!ok) {
                continue;
            }
            uint32_t tq = 1u + t.prseg + t.phseg1 + t.phseg2;
            CHECK(crystals[c] / (2u * t.brp * tq) == bps);
            CHECK(crystals[c] % (2u * t.brp * tq) == 0);
            CHECK(t.brp >= 1 && t.brp <= 64);
            CHECK(t.prseg >= 1 && t.prseg <= 8);
            CHECK(t.phseg1 >= 1 && t.phseg1 <= 8);
            CHECK(t.phseg2 >= 1 && t.phseg2 <= 8);
            CHECK(t.sjw >= 1 && t.sjw <= 4);
            if (tq > 4) {
                CHECK(t.phseg2 >= 2);
                CHECK(t.sjw < t.phseg2);
                CHECK(t.prseg + t.phseg1 >= t.phseg2);
                /* Sample point between 60 % and 90 % of the bit. */
                uint32_t sp = (1u + t.prseg + t.phseg1) * 100u / tq;
                CHECK(sp >= 60 && sp <= 90);
            }
        }
    }
    xl2515_timing_t t;
    CHECK(!xl2515_calc_timing(16000000, 0, &t));
    CHECK(!xl2515_calc_timing(16000000, 333000, &t));
}

/* ------------------------------------------------------------ config -- */

#ifndef NO_CJSON
static void test_config_json(void)
{
    config_t cfg;
    config_defaults(&cfg);
    config_default_filters();
    CHECK(cfg.bitrate1 == 250 && cfg.bitrate2 == 250 && cfg.forwarding && !cfg.listen_only1);
    CHECK(filter_count() == 2 && filter_blocked(0x182, false) && filter_blocked(0x427, false));

    cfg.bitrate1 = 500;
    cfg.bitrate2 = 125;
    cfg.listen_only1 = true;
    cfg.forwarding = false;
    cfg.xtal_mhz = 8;
    strcpy(cfg.wifi_ssid, "My \"Net\"");
    strcpy(cfg.wifi_pass, "secret");
    strcpy(cfg.admin_hash, "abc");
    filter_add(0x18FF50E5, true);

    char *json = config_to_json(&cfg);
    CHECK(json != NULL);
    /* Secrets never leave the device. */
    CHECK(strstr(json, "secret") == NULL);
    CHECK(strstr(json, "abc") == NULL);

    config_t back;
    config_defaults(&back);
    filter_clear();
    char err[96] = "";
    CHECK(config_from_json(json, &back, err, sizeof(err)));
    CHECK(back.bitrate1 == 500 && back.bitrate2 == 125);
    CHECK(back.listen_only1 && !back.forwarding && back.xtal_mhz == 8);
    CHECK(strcmp(back.wifi_ssid, "My \"Net\"") == 0);
    CHECK(filter_count() == 3);
    CHECK(filter_blocked(0x182, false) && filter_blocked(0x427, false) && filter_blocked(0x18FF50E5, true));
    free(json);

    /* Partial documents only change what they mention. */
    CHECK(config_from_json("{\"bus2\":{\"bitrate\":1000}}", &back, err, sizeof(err)));
    CHECK(back.bitrate2 == 1000 && back.bitrate1 == 500 && filter_count() == 3);

    /* IDs may be numbers or hex strings. */
    CHECK(config_from_json("{\"filters\":[{\"id\":256},{\"id\":\"0x1ABCDEF\",\"ext\":true},{\"id\":\"7ff\"}]}",
                           &back, err, sizeof(err)));
    CHECK(filter_count() == 3);
    CHECK(filter_blocked(0x100, false) && filter_blocked(0x1ABCDEF, true) && filter_blocked(0x7FF, false));

    /* Anything invalid is rejected as a whole. */
    config_t before = back;
    static const char *const bad[] = {
        "not json",
        "[1,2,3]",
        "{\"bus1\":{\"bitrate\":333}}",
        "{\"bus1\":{\"bitrate\":500},\"bus2\":{\"xtal_mhz\":7}}",
        "{\"forwarding\":\"yes\"}",
        "{\"bus1\":{\"bitrate\":125},\"filters\":[{\"id\":\"0x800\"}]}",
        "{\"filters\":[{\"id\":\"xyz\"}]}",
        "{\"filters\":[{\"id\":536870912,\"ext\":true}]}",
        "{\"filters\":{}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err[0] = '\0';
        CHECK(!config_from_json(bad[i], &back, err, sizeof(err)));
        CHECK(err[0] != '\0');
        CHECK(memcmp(&before, &back, sizeof(back)) == 0);
        CHECK(filter_count() == 3 && filter_blocked(0x1ABCDEF, true));
    }

    uint32_t id;
    CHECK(config_parse_hex_id("0x182", &id) && id == 0x182);
    CHECK(config_parse_hex_id("1fffffff", &id) && id == 0x1FFFFFFF);
    CHECK(!config_parse_hex_id("20000000", &id));
    CHECK(!config_parse_hex_id("", &id));
    CHECK(!config_parse_hex_id("12z", &id));
}
#endif

int main(void)
{
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"filter: standard IDs", test_filter_basic},
        {"filter: extended IDs", test_filter_extended},
        {"filter: replace and raw export", test_filter_replace_and_raw},
        {"filter: lookup during edits", test_filter_concurrent},
        {"ring buffer", test_ring},
        {"XL2515 bit timing", test_timing},
#ifndef NO_CJSON
        {"config JSON round trip", test_config_json},
#endif
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failures;
        printf("%s\n", tests[i].name);
        tests[i].fn();
        printf("  %s\n", g_failures == before ? "ok" : "FAILED");
    }
#ifdef NO_CJSON
    printf("config JSON round trip\n  skipped (cJSON source not found)\n");
#endif
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
