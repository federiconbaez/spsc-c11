/* SPDX-License-Identifier: Apache-2.0 */
/* usage: bench_spsc [CPU_PRODUCER CPU_CONSUMER [ITEMS]]
 * Throughput (Mitems/s) and ping-pong round trip (ns), median of 7 runs.
 * Compares the SPSC ring (single and batched ops) with a mutex-protected ring. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "spsc.h"

#define CAP 4096
static uint64_t ITEMS = 20000000ull;
#define RUNS 7

static int cpu_a = 0, cpu_b = 1;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((size_t)cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* --- mutex baseline --- */
typedef struct {
    pthread_mutex_t mu;
    uint64_t buf[CAP];
    size_t head, tail;
} mutex_ring;

static int mr_push(mutex_ring *m, uint64_t v) {
    int ok = 0;
    pthread_mutex_lock(&m->mu);
    if (m->head - m->tail < CAP) {
        m->buf[m->head++ % CAP] = v;
        ok = 1;
    }
    pthread_mutex_unlock(&m->mu);
    return ok;
}
static int mr_pop(mutex_ring *m, uint64_t *v) {
    int ok = 0;
    pthread_mutex_lock(&m->mu);
    if (m->head != m->tail) {
        *v = m->buf[m->tail++ % CAP];
        ok = 1;
    }
    pthread_mutex_unlock(&m->mu);
    return ok;
}

typedef enum { SINGLE, BATCH, MUTEX } mode;
typedef struct {
    mode m;
    spsc_ring *r;
    mutex_ring *mr;
    uint64_t sum;
} ctx;

static void *prod(void *arg) {
    ctx *c = arg;
    pin(cpu_a);
    if (c->m == SINGLE) {
        for (uint64_t i = 0; i < ITEMS; i++)
            while (!spsc_push(c->r, &i)) {
            }
    } else if (c->m == BATCH) {
        uint64_t b[64];
        for (uint64_t i = 0; i < ITEMS; i += 64) {
            for (int j = 0; j < 64; j++) b[j] = i + (uint64_t)j;
            size_t done = 0;
            while (done < 64) done += spsc_push_n(c->r, b + done, 64 - done);
        }
    } else {
        for (uint64_t i = 0; i < ITEMS; i++)
            while (!mr_push(c->mr, i)) {
            }
    }
    return NULL;
}

static double throughput(mode m) {
    static uint64_t storage[CAP];
    static mutex_ring mr;
    spsc_ring r;
    spsc_init(&r, storage, CAP, sizeof(uint64_t));
    pthread_mutex_init(&mr.mu, NULL);
    mr.head = mr.tail = 0;
    ctx c = {m, &r, &mr, 0};
    pin(cpu_b);
    pthread_t t;
    double t0 = now();
    pthread_create(&t, NULL, prod, &c);
    uint64_t v, sum = 0, got = 0;
    if (m == SINGLE) {
        while (got < ITEMS)
            if (spsc_pop(&r, &v)) sum += v, got++;
    } else if (m == BATCH) {
        uint64_t b[64];
        while (got < ITEMS) {
            size_t k = spsc_pop_n(&r, b, 64);
            for (size_t j = 0; j < k; j++) sum += b[j];
            got += k;
        }
    } else {
        while (got < ITEMS)
            if (mr_pop(&mr, &v)) sum += v, got++;
    }
    pthread_join(t, NULL);
    double dt = now() - t0;
    if (sum != ITEMS * (ITEMS - 1) / 2) {
        fprintf(stderr, "checksum mismatch\n");
        exit(1);
    }
    return (double)ITEMS / dt / 1e6;
}

/* --- ping-pong latency over two rings --- */
#define PINGS 2000000
static spsc_ring ping, pong;
static uint64_t ping_buf[CAP], pong_buf[CAP];

static void *echo(void *arg) {
    (void)arg;
    pin(cpu_a);
    uint64_t v;
    for (int i = 0; i < PINGS; i++) {
        while (!spsc_pop(&ping, &v)) {
        }
        while (!spsc_push(&pong, &v)) {
        }
    }
    return NULL;
}

static double roundtrip_ns(void) {
    spsc_init(&ping, ping_buf, CAP, sizeof(uint64_t));
    spsc_init(&pong, pong_buf, CAP, sizeof(uint64_t));
    pin(cpu_b);
    pthread_t t;
    pthread_create(&t, NULL, echo, NULL);
    double t0 = now();
    for (uint64_t i = 0; i < PINGS; i++) {
        uint64_t v;
        while (!spsc_push(&ping, &i)) {
        }
        while (!spsc_pop(&pong, &v)) {
        }
    }
    double dt = now() - t0;
    pthread_join(t, NULL);
    return dt / PINGS * 1e9;
}

static double median(double (*f)(mode), mode m) {
    double v[RUNS];
    for (int i = 0; i < RUNS; i++) v[i] = f(m);
    qsort(v, RUNS, sizeof v[0], cmp);
    return v[RUNS / 2];
}

static double rt(mode m) {
    (void)m;
    return roundtrip_ns();
}

int main(int argc, char **argv) {
    if (argc > 2) cpu_a = atoi(argv[1]), cpu_b = atoi(argv[2]);
    if (argc > 3) ITEMS = strtoull(argv[3], NULL, 10) / 64 * 64;
    printf("cpus %d -> %d, capacity %d, %llu items, median of %d\n", cpu_a, cpu_b, CAP, (unsigned long long)ITEMS, RUNS);
    printf("spsc push/pop      %8.1f Mitems/s\n", median(throughput, SINGLE));
    printf("spsc batch 64      %8.1f Mitems/s\n", median(throughput, BATCH));
    printf("mutex ring         %8.1f Mitems/s\n", median(throughput, MUTEX));
    printf("ping-pong RTT      %8.1f ns\n", median(rt, SINGLE));
    return 0;
}
