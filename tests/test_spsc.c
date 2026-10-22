/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "spsc.h"

static int failures;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static void test_init_validation(void) {
    spsc_ring r;
    int buf[8];
    CHECK(spsc_init(&r, buf, 0, sizeof(int)) == -1);
    CHECK(spsc_init(&r, buf, 3, sizeof(int)) == -1);
    CHECK(spsc_init(&r, buf, 6, sizeof(int)) == -1);
    CHECK(spsc_init(&r, buf, 8, 0) == -1);
    CHECK(spsc_init(&r, buf, 8, sizeof(int)) == 0);
    CHECK(spsc_capacity(&r) == 8);
    CHECK(spsc_init(&r, buf, 1, sizeof(int)) == 0);
}

static void test_fifo_full_empty(void) {
    spsc_ring r;
    int buf[4], v;
    spsc_init(&r, buf, 4, sizeof(int));
    CHECK(!spsc_pop(&r, &v));
    CHECK(spsc_front(&r) == NULL);
    for (int i = 0; i < 4; i++) CHECK(spsc_push(&r, &i));
    int extra = 99;
    CHECK(!spsc_push(&r, &extra));
    CHECK(spsc_size(&r) == 4);
    for (int i = 0; i < 4; i++) {
        CHECK(spsc_pop(&r, &v));
        CHECK(v == i);
    }
    CHECK(!spsc_pop(&r, &v));
    /* interleaved across many laps */
    int next_in = 0, next_out = 0;
    for (int step = 0; step < 1000; step++) {
        int k = step % 5;
        for (int j = 0; j < k; j++)
            if (spsc_push(&r, &next_in)) next_in++;
        for (int j = 0; j < (step % 3) + 1; j++)
            if (spsc_pop(&r, &v)) CHECK(v == next_out++);
    }
    while (spsc_pop(&r, &v)) CHECK(v == next_out++);
    CHECK(next_in == next_out);
}

static void test_capacity_one(void) {
    spsc_ring r;
    uint64_t slot, v = 7, out = 0;
    spsc_init(&r, &slot, 1, sizeof slot);
    CHECK(spsc_push(&r, &v));
    CHECK(!spsc_push(&r, &v));
    CHECK(spsc_pop(&r, &out) && out == 7);
    CHECK(!spsc_pop(&r, &out));
}

static void test_batch_partial_and_buffer_wrap(void) {
    spsc_ring r;
    int buf[8], in[16], out[16];
    for (int i = 0; i < 16; i++) in[i] = i;
    spsc_init(&r, buf, 8, sizeof(int));
    CHECK(spsc_push_n(&r, in, 5) == 5);
    CHECK(spsc_pop_n(&r, out, 3) == 3); /* tail at 3 */
    CHECK(out[0] == 0 && out[2] == 2);
    /* 6 free; asking for 10 writes 6 and wraps the buffer */
    CHECK(spsc_push_n(&r, in + 5, 10) == 6);
    CHECK(spsc_push_n(&r, in, 1) == 0);
    CHECK(spsc_pop_n(&r, out, 16) == 8);
    for (int i = 0; i < 8; i++) CHECK(out[i] == i + 3);
    CHECK(spsc_pop_n(&r, out, 4) == 0);
    CHECK(spsc_push_n(&r, in, 0) == 0);
}

static void test_counter_wraparound(void) {
    spsc_ring r;
    uint32_t buf[8], v;
    spsc_init(&r, buf, 8, sizeof v);
    spsc_reset_at(&r, SIZE_MAX - 4); /* head/tail overflow after 5 pushes */
    uint32_t next_in = 0, next_out = 0;
    for (int lap = 0; lap < 50; lap++) {
        uint32_t tmp[8];
        size_t want = (size_t)(lap % 8) + 1;
        for (size_t j = 0; j < want; j++) tmp[j] = next_in + (uint32_t)j;
        size_t got = spsc_push_n(&r, tmp, want);
        next_in += (uint32_t)got;
        CHECK(spsc_size(&r) <= 8);
        size_t popped = spsc_pop_n(&r, tmp, (size_t)(lap % 3) + 2);
        for (size_t j = 0; j < popped; j++) CHECK(tmp[j] == next_out++);
    }
    while (spsc_pop(&r, &v)) CHECK(v == next_out++);
    CHECK(next_in == next_out);
    CHECK(atomic_load(&r.head) < 1000); /* really wrapped past zero */
}

static void test_front_consume(void) {
    spsc_ring r;
    char buf[4][16];
    spsc_init(&r, buf, 4, 16);
    char msg[16] = "hello";
    spsc_push(&r, msg);
    const char *p = spsc_front(&r);
    CHECK(p != NULL && strcmp(p, "hello") == 0);
    CHECK(spsc_front(&r) == p); /* peeking does not consume */
    spsc_consume(&r);
    CHECK(spsc_front(&r) == NULL);
}

/* ---- two-thread stress ---- */

typedef struct {
    uint64_t seq;
    uint64_t twice;
    uint64_t check; /* seq ^ twice ^ magic: detects torn element copies */
} item;

#define MAGIC 0x9E3779B97F4A7C15ull

typedef struct {
    spsc_ring *r;
    uint64_t n;
    unsigned seed;
} worker;

static unsigned xorshift(unsigned *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void *producer(void *arg) {
    worker *w = arg;
    item batch[32];
    uint64_t next = 0;
    while (next < w->n) {
        size_t k = (xorshift(&w->seed) % 32) + 1;
        if (k > w->n - next) k = (size_t)(w->n - next);
        for (size_t j = 0; j < k; j++) {
            uint64_t s = next + j;
            batch[j] = (item){s, s * 2, s ^ (s * 2) ^ MAGIC};
        }
        size_t done = 0;
        while (done < k) done += spsc_push_n(w->r, batch + done, k - done);
        next += k;
    }
    return NULL;
}

static uint64_t consumer_errors;

static void *consumer(void *arg) {
    worker *w = arg;
    item batch[32];
    uint64_t expect = 0;
    while (expect < w->n) {
        size_t k = (xorshift(&w->seed) % 32) + 1;
        size_t got;
        if (k == 1) {
            const item *it = spsc_front(w->r);
            if (!it) continue;
            batch[0] = *it;
            spsc_consume(w->r);
            got = 1;
        } else {
            got = spsc_pop_n(w->r, batch, k);
        }
        for (size_t j = 0; j < got; j++, expect++) {
            const item *it = &batch[j];
            if (it->seq != expect || it->twice != expect * 2 || it->check != (it->seq ^ it->twice ^ MAGIC))
                consumer_errors++;
        }
    }
    return NULL;
}

static void test_two_thread_stress(uint64_t n, size_t cap) {
    item *storage = malloc(cap * sizeof(item));
    spsc_ring r;
    spsc_init(&r, storage, cap, sizeof(item));
    worker p = {&r, n, 12345u}, c = {&r, n, 67890u};
    pthread_t tp, tc;
    consumer_errors = 0;
    pthread_create(&tc, NULL, consumer, &c);
    pthread_create(&tp, NULL, producer, &p);
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
    CHECK(consumer_errors == 0);
    CHECK(spsc_size(&r) == 0);
    printf("stress: %" PRIu64 " items through capacity %zu, %" PRIu64 " errors\n", n, cap, consumer_errors);
    free(storage);
}

int main(int argc, char **argv) {
    uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 2000000;
    test_init_validation();
    test_fifo_full_empty();
    test_capacity_one();
    test_batch_partial_and_buffer_wrap();
    test_counter_wraparound();
    test_front_consume();
    test_two_thread_stress(n, 1024);
    test_two_thread_stress(n / 4, 2); /* tiny ring: constant full/empty contention */
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
