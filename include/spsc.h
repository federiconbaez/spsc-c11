/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Lock-free single-producer / single-consumer ring buffer in C11.
 *
 * - Capacity is a power of two; head/tail are free-running size_t counters, so
 *   "full" is head - tail == capacity and wraparound of the counters is benign.
 * - The producer owns `head`, the consumer owns `tail`; each lives on its own
 *   cache line together with a private cached copy of the other side's index,
 *   so the hot path touches the shared line only when the cache says full/empty.
 * - Synchronisation: the producer publishes with a release store of `head` and
 *   the consumer observes it with an acquire load (and symmetrically for
 *   `tail`), which orders the element copy before the index update.
 *
 * Exactly one thread may call the producer functions and exactly one thread
 * the consumer functions. Elements are copied by memcpy (trivially copyable).
 */
#ifndef SPSC_H
#define SPSC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef SPSC_CACHELINE
#define SPSC_CACHELINE 64
#endif

/* Orderings used for the cross-thread index handoff. Overridable only so the
 * test suite can prove that ThreadSanitizer catches a relaxed (broken) build. */
#ifndef SPSC_ACQUIRE
#define SPSC_ACQUIRE memory_order_acquire
#endif
#ifndef SPSC_RELEASE
#define SPSC_RELEASE memory_order_release
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spsc_ring {
    /* producer line */
    _Alignas(SPSC_CACHELINE) atomic_size_t head;
    size_t cached_tail;
    /* consumer line */
    _Alignas(SPSC_CACHELINE) atomic_size_t tail;
    size_t cached_head;
    /* read-only after init */
    _Alignas(SPSC_CACHELINE) size_t mask;
    size_t elem_size;
    unsigned char *buf;
} spsc_ring;

/* Returns 0 on success, -1 if capacity is not a non-zero power of two or
 * elem_size is 0. `storage` must hold capacity * elem_size bytes. */
static inline int spsc_init(spsc_ring *r, void *storage, size_t capacity, size_t elem_size) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0 || elem_size == 0) return -1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    r->cached_tail = 0;
    r->cached_head = 0;
    r->mask = capacity - 1;
    r->elem_size = elem_size;
    r->buf = (unsigned char *)storage;
    return 0;
}

/* Test hook: start both counters at `start` (used to exercise wraparound). */
static inline void spsc_reset_at(spsc_ring *r, size_t start) {
    atomic_store_explicit(&r->head, start, memory_order_relaxed);
    atomic_store_explicit(&r->tail, start, memory_order_relaxed);
    r->cached_tail = start;
    r->cached_head = start;
}

static inline size_t spsc_capacity(const spsc_ring *r) { return r->mask + 1; }

/* ---- producer side ---- */

/* Pushes up to n elements; returns how many were written (0..n). */
static inline size_t spsc_push_n(spsc_ring *r, const void *src, size_t n) {
    const size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    const size_t cap = r->mask + 1;
    size_t free_slots = cap - (head - r->cached_tail);
    if (free_slots < n) {
        r->cached_tail = atomic_load_explicit(&r->tail, SPSC_ACQUIRE);
        free_slots = cap - (head - r->cached_tail);
        if (n > free_slots) n = free_slots;
        if (n == 0) return 0;
    }
    const size_t idx = head & r->mask;
    const size_t first = n < cap - idx ? n : cap - idx;
    const size_t es = r->elem_size;
    memcpy(r->buf + idx * es, src, first * es);
    if (n > first) memcpy(r->buf, (const unsigned char *)src + first * es, (n - first) * es);
    atomic_store_explicit(&r->head, head + n, SPSC_RELEASE);
    return n;
}

static inline bool spsc_push(spsc_ring *r, const void *elem) { return spsc_push_n(r, elem, 1) == 1; }

/* ---- consumer side ---- */

/* Pops up to n elements into dst; returns how many were read (0..n). */
static inline size_t spsc_pop_n(spsc_ring *r, void *dst, size_t n) {
    const size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t avail = r->cached_head - tail;
    if (avail < n) {
        r->cached_head = atomic_load_explicit(&r->head, SPSC_ACQUIRE);
        avail = r->cached_head - tail;
        if (n > avail) n = avail;
        if (n == 0) return 0;
    }
    const size_t cap = r->mask + 1;
    const size_t idx = tail & r->mask;
    const size_t first = n < cap - idx ? n : cap - idx;
    const size_t es = r->elem_size;
    memcpy(dst, r->buf + idx * es, first * es);
    if (n > first) memcpy((unsigned char *)dst + first * es, r->buf, (n - first) * es);
    atomic_store_explicit(&r->tail, tail + n, SPSC_RELEASE);
    return n;
}

static inline bool spsc_pop(spsc_ring *r, void *elem) { return spsc_pop_n(r, elem, 1) == 1; }

/* Pointer to the oldest element without consuming it, or NULL if empty.
 * Valid until the next spsc_pop/spsc_consume on this ring. */
static inline const void *spsc_front(spsc_ring *r) {
    const size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (r->cached_head == tail) {
        r->cached_head = atomic_load_explicit(&r->head, SPSC_ACQUIRE);
        if (r->cached_head == tail) return NULL;
    }
    return r->buf + (tail & r->mask) * r->elem_size;
}

/* Drops one element previously observed through spsc_front. */
static inline void spsc_consume(spsc_ring *r) {
    const size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    atomic_store_explicit(&r->tail, tail + 1, SPSC_RELEASE);
}

/* Approximate occupancy; exact only when both sides are quiescent. */
static inline size_t spsc_size(spsc_ring *r) {
    const size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    const size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    return h - t;
}

#ifdef __cplusplus
}
#endif
#endif /* SPSC_H */
