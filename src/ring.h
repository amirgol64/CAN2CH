#pragma once

/*
 * Lock-free single-producer / single-consumer ring buffer of CAN frames.
 * The producer may only call ring_push(); the consumer may only call
 * ring_pop() and ring_flush(). ring_count() is safe from anywhere.
 */

#include <stdatomic.h>
#include <stdbool.h>

#include "can_frame.h"

/*
 * Must be a power of two, and larger than the CAN1 driver queue: after a
 * pause the receive task empties that whole queue into this ring at once.
 */
#define RING_SIZE 2048u

typedef struct {
    can_frame_t buf[RING_SIZE];
    atomic_uint head; /* written by the producer */
    atomic_uint tail; /* written by the consumer */
} ring_t;

static inline bool ring_push(ring_t *r, const can_frame_t *f)
{
    unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail >= RING_SIZE) {
        return false;
    }
    r->buf[head & (RING_SIZE - 1)] = *f;
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return true;
}

static inline bool ring_pop(ring_t *r, can_frame_t *f)
{
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (head == tail) {
        return false;
    }
    *f = r->buf[tail & (RING_SIZE - 1)];
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return true;
}

static inline unsigned ring_count(ring_t *r)
{
    unsigned head = atomic_load_explicit(&r->head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return head - tail;
}

/* Consumer side: discard everything queued. Returns the number discarded. */
static inline unsigned ring_flush(ring_t *r)
{
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&r->head, memory_order_acquire);
    atomic_store_explicit(&r->tail, head, memory_order_release);
    return head - tail;
}
