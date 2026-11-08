/*
 * fl-supervisor: starts the system, notices failures, restarts what failed,
 * and timestamps every step so recovery can be measured.
 *
 *   fl-supervisor <system.conf> [bin-dir]
 *
 * system.conf, one process per line:
 *   name  program  priority  heartbeat_ms  restart_limit  faultable
 *
 * Crashes: a reaper thread blocks in waitpid() and pulses the main loop with the
 * dead pid, so a crash is seen as soon as the kernel cleans up the process.
 * Hangs: every process sends heartbeat pulses. A 5 ms timer checks each
 * deadline. A process that misses it is killed and restarted like a crash.
 * Recovery ends when the replacement sends its first heartbeat.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "fl.h"

#define CHECK_PERIOD_NS 5000000ULL
#define DEATH_RING 32

extern char **environ;

static struct fl_state *s;
static int self_coid = -1;
static const char *bin_dir = "/data/fl/bin";

/* Deaths the reaper thread has seen but the main loop hasn't handled yet. */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { pid_t pid; int status; uint64_t t_ns; } deaths[DEATH_RING];
static unsigned d_head, d_tail;

static int launch(int i)
{
    struct fl_proc *p = &s->proc[i];
    char path[128], env_proc[24];
    snprintf(path, sizeof(path), "%s/%s", bin_dir, p->path);
    snprintf(env_proc, sizeof(env_proc), "FL_PROC=%d", i);

    /* Child environment: ours plus FL_PROC. */
    int n = 0;
    while (environ && environ[n]) n++;
    char **envp = calloc(n + 2, sizeof(char *));
    for (int k = 0; k < n; k++) envp[k] = environ[k];
    envp[n] = env_proc;

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    struct sched_param sp = { .sched_priority = p->prio };
    posix_spawnattr_setschedpolicy(&attr, SCHED_FIFO);
    posix_spawnattr_setschedparam(&attr, &sp);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSCHEDPARAM);

    char *argv[] = { p->path, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, path, NULL, &attr, argv, envp);
    posix_spawnattr_destroy(&attr);
    free(envp);
    if (rc != 0) {
        fprintf(stderr, "spawn %s: %s\n", path, strerror(rc));
        p->state = FL_FAILED;
        return -1;
    }
    p->pid = pid;
    p->started_ns = fl_now_ns();
    p->last_hb_ns = 0;
    p->state = FL_STARTING;
    return 0;
}

static void *reaper(void *arg)
{
    (void)arg;
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid == -1) {
            if (errno == ECHILD) {
                usleep(1000);
            }
            continue;
        }
        uint64_t t = fl_now_ns();
        pthread_mutex_lock(&ring_lock);
        if (d_head - d_tail >= DEATH_RING) {
            d_tail++;   /* main loop far behind: drop the oldest rather than overwrite silently */
            fprintf(stderr, "fl-supervisor: death queue full, dropped one\n");
        }
        deaths[d_head % DEATH_RING].pid = pid;
        deaths[d_head % DEATH_RING].status = status;
        deaths[d_head % DEATH_RING].t_ns = t;
        d_head++;
        pthread_mutex_unlock(&ring_lock);
        MsgSendPulse(self_coid, -1, FL_PULSE_CHILD, pid);
    }
    return NULL;
}

static void restart(int i, const char *why)
{
    struct fl_proc *p = &s->proc[i];
    if ((int)p->restarts >= p->restart_limit) {
        p->state = FL_FAILED;
        fl_event(s, FL_EV_GAVE_UP, i, p->pid, p->restarts, "%s: restart limit reached", p->name);
        return;
    }
    p->restarts++;
    p->state = FL_RESTARTING;
    if (launch(i) == 0) {
        fl_event(s, FL_EV_RESTART, i, p->pid, p->restarts, "%s restarted after %s", p->name, why);
    } else {
        fl_event(s, FL_EV_GAVE_UP, i, 0, p->restarts, "%s could not be restarted", p->name);
    }
}

static void handle_deaths(void)
{
    for (;;) {
        pthread_mutex_lock(&ring_lock);
        if (d_tail == d_head) {
            pthread_mutex_unlock(&ring_lock);
            return;
        }
        pid_t pid = deaths[d_tail % DEATH_RING].pid;
        int status = deaths[d_tail % DEATH_RING].status;
        uint64_t t = deaths[d_tail % DEATH_RING].t_ns;
        d_tail++;
        pthread_mutex_unlock(&ring_lock);

        for (int i = 0; i < s->nproc; i++) {
            struct fl_proc *p = &s->proc[i];
            if (p->pid != pid) {
                continue;
            }
            int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            if (p->last_cause == FL_EV_HANG && p->detect_ns) {
                /* We killed it ourselves after a missed heartbeat; already logged. */
            } else {
                p->detect_ns = t;
                p->last_cause = FL_EV_DEATH;
                if (p->fault_ns) {
                    double ms = (double)(t - p->fault_ns) / 1e6;
                    fl_event(s, FL_EV_DEATH, i, pid, sig, "%s died (signal %d), seen %.2f ms after fault",
                             p->name, sig, ms);
                } else {
                    fl_event(s, FL_EV_DEATH, i, pid, sig, "%s died (signal %d)", p->name, sig);
                }
            }
            restart(i, p->last_cause == FL_EV_HANG ? "hang" : "crash");
            break;
        }
    }
}

static void check_heartbeats(void)
{
    uint64_t now = fl_now_ns();
    for (int i = 0; i < s->nproc; i++) {
        struct fl_proc *p = &s->proc[i];
        if (p->heartbeat_ms <= 0 || p->state != FL_RUNNING) {
            continue;
        }
        uint64_t limit = (uint64_t)p->heartbeat_ms * 1000000ULL;
        if (now - p->last_hb_ns > limit) {
            p->detect_ns = now;
            p->last_cause = FL_EV_HANG;
            int64_t lat = p->fault_ns ? (int64_t)(now - p->fault_ns) / 1000 : -1;
            fl_event(s, FL_EV_HANG, i, p->pid, (int)lat, "%s missed its %d ms heartbeat",
                     p->name, p->heartbeat_ms);
            p->state = FL_RESTARTING;
            kill(p->pid, SIGKILL);   /* also ends a SIGSTOPped process; the reaper then restarts it */
        }
    }
}

/* A heartbeat carries the sender's pid. A pulse still queued from a process that has
 * since died no longer matches, so it can't mark the replacement as ready. */
static void on_heartbeat(pid_t pid)
{
    int i = 0;
    while (i < s->nproc && s->proc[i].pid != pid) {
        i++;
    }
    if (i == s->nproc) {
        return;
    }
    struct fl_proc *p = &s->proc[i];
    uint64_t now = fl_now_ns();
    p->last_hb_ns = now;
    if (p->state == FL_STARTING || p->state == FL_RESTARTING) {
        int first_start = p->restarts == 0 && p->detect_ns == 0;
        p->state = FL_RUNNING;
        if (first_start) {
            fl_event(s, FL_EV_START, i, p->pid, 0, "%s running", p->name);
        } else {
            uint64_t from = p->fault_ns ? p->fault_ns : p->detect_ns;
            int us = (int)((now - from) / 1000);
            fl_event(s, FL_EV_READY, i, p->pid, us, "%s back %.2f ms after %s",
                     p->name, us / 1000.0, p->fault_ns ? "fault" : "failure");
            p->fault_ns = 0;
            p->detect_ns = 0;
            p->last_cause = 0;
        }
    }
}

static int load_conf(const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f) {
        perror(file);
        return -1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && s->nproc < FL_MAXPROC) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        struct fl_proc *p = &s->proc[s->nproc];
        if (sscanf(line, "%15s %63s %d %d %d %d", p->name, p->path, &p->prio,
                   &p->heartbeat_ms, &p->restart_limit, &p->faultable) == 6) {
            s->nproc++;
        }
    }
    fclose(f);
    return s->nproc > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fl-supervisor <system.conf> [bin-dir]\n");
        return 1;
    }
    if (argc > 2) {
        bin_dir = argv[2];
    }
    s = fl_map(1);
    if (!s || load_conf(argv[1]) == -1) {
        return 1;
    }
    fl_set_fifo(55);

    /* A crashed process is only gone once the dumper has written its core file,
     * which delays detection. Children inherit this limit; set FL_CORE=1 to keep dumps. */
    if (!getenv("FL_CORE")) {
        struct rlimit rl = { 0, 0 };
        setrlimit(RLIMIT_CORE, &rl);
    }

    name_attach_t *att = name_attach(NULL, FL_SUPERVISOR, 0);
    if (!att) {
        perror("name_attach " FL_SUPERVISOR);
        return 1;
    }
    self_coid = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);
    timer_t tid;
    if (self_coid == -1 || fl_periodic_pulse(self_coid, FL_PULSE_TICK, CHECK_PERIOD_NS, &tid) == -1) {
        return 1;
    }

    pthread_attr_t ta;
    pthread_attr_init(&ta);
    struct sched_param sp = { .sched_priority = 56 };
    pthread_attr_setinheritsched(&ta, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&ta, SCHED_FIFO);
    pthread_attr_setschedparam(&ta, &sp);
    pthread_t rt;
    pthread_create(&rt, &ta, reaper, NULL);

    for (int i = 0; i < s->nproc; i++) {
        launch(i);
        usleep(20000);   /* start in order so servers exist before their clients */
    }
    printf("fl-supervisor: %d processes started\n", s->nproc);
    fflush(stdout);

    union { uint16_t type; struct _pulse pulse; char raw[256]; } msg;
    for (;;) {
        rcvid_t rcvid = MsgReceive(att->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            continue;
        }
        if (rcvid > 0) {
            if (msg.type == _IO_CONNECT) {
                MsgReply(rcvid, EOK, NULL, 0);   /* name_open() handshake from a client */
            } else {
                MsgError(rcvid, ENOSYS);
            }
            continue;
        }
        switch (msg.pulse.code) {
        case FL_PULSE_HEARTBEAT:
            on_heartbeat((pid_t)msg.pulse.value.sival_int);
            break;
        case FL_PULSE_TICK:
            check_heartbeats();
            break;
        case FL_PULSE_CHILD:
            handle_deaths();
            break;
        case _PULSE_CODE_DISCONNECT:
            ConnectDetach(msg.pulse.scoid);
            break;
        default:
            break;
        }
    }
}
