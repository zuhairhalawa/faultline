/*
 * client: pings the server with a timeout on every request.
 *
 *   client [-t TIMEOUT_MS] [-n COUNT] [-w]
 *
 *   -t TIMEOUT_MS  TimerTimeout for each MsgSend (default 5)
 *   -n COUNT       how many pings, or with -w how many 100 ms windows (default 10)
 *   -w             control-loop mode: a 100 Hz loop that never calls MsgSend
 *                  itself; a worker thread does. Each 100 ms it prints the
 *                  loop's worst lateness and how many answers came back.
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "ping.h"

static int coid = -1;
static uint64_t timeout_ns;

/* One request, bounded (we hope) by TimerTimeout. Returns 0 or an errno value. */
static int ping(uint32_t seq)
{
    struct ping_req req = { PING_TYPE, 0, seq };
    struct ping_reply rep;
    uint64_t t = timeout_ns;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &t, NULL);
    return MsgSend(coid, &req, sizeof(req), &rep, sizeof(rep)) == -1 ? errno : 0;
}

/* ---- control-loop mode ---- */
static sem_t go;
static volatile int busy;
static volatile unsigned answers;

static void *worker(void *arg)
{
    (void)arg;
    for (uint32_t seq = 1;; seq++) {
        sem_wait(&go);
        if (ping(seq) == 0) answers++;
        busy = 0;
    }
    return NULL;
}

static void control_loop(int windows)
{
    sem_init(&go, 0, 0);
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double worst = 0;
    for (int tick = 1; tick <= windows * 10; tick++) {
        next.tv_nsec += 10000000L;                        /* 100 Hz */
        if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        double late = now_ms() - (next.tv_sec * 1e3 + next.tv_nsec / 1e6);
        if (late > worst) worst = late;

        if (!busy) { busy = 1; sem_post(&go); }           /* never wait for the server here */

        if (tick % 10 == 0) {
            printf("loop: 100 ms window %2d, worst tick %.2f ms late, %u answers\n", tick / 10, worst, answers);
            fflush(stdout);
            worst = 0;
            answers = 0;
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
        else { fprintf(stderr, "usage: client [-t TIMEOUT_MS] [-n COUNT] [-w]\n"); return 2; }
    }
    timeout_ns = (uint64_t)timeout_ms * 1000000ULL;

    coid = name_open(PING_NAME, 0);
    if (coid == -1) { perror("name_open " PING_NAME " (is the server running?)"); return 1; }
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
