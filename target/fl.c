/* Faultline: helpers shared by every target process. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/siginfo.h>
#include "fl.h"

uint64_t fl_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct fl_state *fl_map(int create)
{
    int fd = shm_open(FL_SHM_NAME, create ? (O_RDWR | O_CREAT) : O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open " FL_SHM_NAME);
        return NULL;
    }
    if (create && ftruncate(fd, sizeof(struct fl_state)) == -1) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, sizeof(struct fl_state), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    struct fl_state *s = p;
    if (create) {
        memset(s, 0, sizeof(*s));
        s->boot_ns = fl_now_ns();
        s->magic = FL_MAGIC;
    } else if (s->magic != FL_MAGIC) {
        fprintf(stderr, "shared state not initialised (start fl-supervisor first)\n");
        return NULL;
    }
    return s;
}

void fl_event(struct fl_state *s, int type, int proc, int pid, int arg, const char *fmt, ...)
{
    uint32_t slot = __atomic_fetch_add(&s->ev_head, 1, __ATOMIC_ACQ_REL);
    struct fl_event *e = &s->ev[slot % FL_EVENTS];
    e->t_ns = fl_now_ns();
    e->type = type;
    e->proc = proc;
    e->pid = pid;
    e->arg = arg;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
}

int fl_find_proc(struct fl_state *s, const char *name)
{
    for (int i = 0; i < s->nproc; i++) {
        if (strcmp(s->proc[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int fl_my_index(void)
{
    const char *v = getenv("FL_PROC");
    return v ? atoi(v) : -1;
}

int fl_heartbeat_open(void)
{
    return name_open(FL_SUPERVISOR, 0);
}

/* Sends one heartbeat pulse. Reconnects if the supervisor connection was lost. */
void fl_heartbeat(int *coid, int index)
{
    if (index < 0) {
        return;
    }
    if (*coid == -1) {
        *coid = fl_heartbeat_open();
        if (*coid == -1) {
            return;
        }
    }
    if (MsgSendPulse(*coid, -1, FL_PULSE_HEARTBEAT, index) == -1) {
        name_close(*coid);
        *coid = -1;
    }
}

void fl_set_fifo(int prio)
{
    struct sched_param sp = { .sched_priority = prio };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        fprintf(stderr, "pthread_setschedparam(%d): %s\n", prio, strerror(rc));
    }
}

/* Creates a timer that delivers a pulse with the given code to coid every period_ns. */
int fl_periodic_pulse(int coid, int code, uint64_t period_ns, timer_t *out)
{
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, code, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, out) == -1) {
        perror("timer_create");
        return -1;
    }
    struct itimerspec its;
    its.it_value.tv_sec = period_ns / 1000000000ULL;
    its.it_value.tv_nsec = period_ns % 1000000000ULL;
    its.it_interval = its.it_value;
    if (timer_settime(*out, 0, &its, NULL) == -1) {
        perror("timer_settime");
        return -1;
    }
    return 0;
}
