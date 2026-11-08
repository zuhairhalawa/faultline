/*
 * cl-client: pings the server with a timeout on every request.
 *
 *   cl-client [-t TIMEOUT_MS] [-n COUNT] [-w]
 *
 *   -t TIMEOUT_MS  TimerTimeout for each MsgSend (default 5)
 *   -n COUNT       how many pings, or with -w how many 100 ms windows (default 10)
 *   -w             control-loop mode: a 100 Hz SCHED_FIFO loop that never calls
 *                  MsgSend itself; a lower-priority worker thread does. Each 100 ms
 *                  it prints the loop's worst lateness and how many answers came back.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "ping.h"

#define LOOP_PRIO   20   /* the time-critical loop */
#define WORKER_PRIO 15   /* does the MsgSend; may block for as long as the server likes */

static int coid = -1;
static uint64_t timeout_ns;

/* One request, bounded (we hope) by TimerTimeout. Returns 0 or an errno value. */
static int ping(uint32_t seq)
{
    struct ping_req req = { PING_TYPE, 0, seq };
    struct ping_reply rep;
    uint64_t t = timeout_ns;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &t, NULL) == -1) return errno;
    return MsgSend(coid, &req, sizeof(req), &rep, sizeof(rep)) == -1 ? errno : 0;
}

/* ---- control-loop mode ---- */
static sem_t go;
static atomic_int busy;          /* 1 while the worker has a request in flight */
static atomic_uint answers;      /* answers received in the current window */

static void *worker(void *arg)
{
    (void)arg;
    for (uint32_t seq = 1;; seq++) {
        sem_wait(&go);
        if (ping(seq) == 0) atomic_fetch_add(&answers, 1);
        atomic_store(&busy, 0);
    }
    return NULL;
}

static void control_loop(int windows)
{
    struct sched_param sp = { .sched_priority = LOOP_PRIO };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) fprintf(stderr, "loop priority: %s (run as root for SCHED_FIFO %d)\n", strerror(rc), LOOP_PRIO);

    if (sem_init(&go, 0, 0) == -1) { perror("sem_init"); exit(1); }
    pthread_attr_t ta;
    pthread_attr_init(&ta);
    pthread_attr_setinheritsched(&ta, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&ta, SCHED_FIFO);
    struct sched_param wp = { .sched_priority = WORKER_PRIO };
    pthread_attr_setschedparam(&ta, &wp);
    pthread_t t;
    rc = pthread_create(&t, &ta, worker, NULL);
    if (rc != EOK) { fprintf(stderr, "pthread_create: %s\n", strerror(rc)); exit(1); }

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double worst = 0;
    for (int tick = 1; tick <= windows * 10; tick++) {
        next.tv_nsec += 10000000L;                        /* 100 Hz */
        if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        double late = now_ms() - (next.tv_sec * 1e3 + next.tv_nsec / 1e6);
        if (late > worst) worst = late;

        int idle = 0;                                     /* never wait for the server here */
        if (atomic_compare_exchange_strong(&busy, &idle, 1)) sem_post(&go);

        if (tick % 10 == 0) {
            printf("loop: 100 ms window %2d, worst tick %.2f ms late, %u answers\n",
                   tick / 10, worst, atomic_exchange(&answers, 0));
            fflush(stdout);
            worst = 0;
        }
    }
}

int main(int argc, char **argv)
{
    int timeout_ms = 5, count = 10, loop = 0, opt;
    while ((opt = getopt(argc, argv, "t:n:w")) != -1) {
        if (opt == 't') timeout_ms = atoi(optarg);
        else if (opt == 'n') count = atoi(optarg);
        else if (opt == 'w') loop = 1;
        else { fprintf(stderr, "usage: cl-client [-t TIMEOUT_MS] [-n COUNT] [-w]\n"); return 2; }
    }
    timeout_ns = (uint64_t)timeout_ms * 1000000ULL;

    coid = name_open(PING_NAME, 0);
    if (coid == -1) { perror("name_open " PING_NAME " (is cl-server running?)"); return 1; }
    printf("client: pid %d, timeout %d ms\n", getpid(), timeout_ms);
    fflush(stdout);

    if (loop) {
        control_loop(count);
        return 0;
    }
    for (int i = 1; i <= count; i++) {
        double t0 = now_ms();
        int err = ping((uint32_t)i);
        double took = now_ms() - t0;
        if (err == 0) printf("ping %2d: reply after %.1f ms\n", i, took);
        else printf("ping %2d: %s after %.1f ms\n", i, strerror(err), took);
        fflush(stdout);
        usleep(100000);
    }
    return 0;
}
