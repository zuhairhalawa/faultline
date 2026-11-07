/*
 * pump-ctrl: the infusion pump's 100 Hz control loop.
 *
 * The loop never waits on another process. Two worker threads do the I/O:
 * one asks the flow sensor for a reading over QNX message passing, the other
 * writes motor speed to /dev/fl/motor. Each tick the loop starts a new read,
 * uses the newest reading if it is fresh, and hands the new speed to the motor
 * worker.
 *
 * Why: a sensor attached with name_attach() has _NTO_CHF_UNBLOCK set, so a
 * client that is reply-blocked on it is only released when the server answers
 * the unblock pulse. A frozen server never answers, so even a TimerTimeout
 * can't free the caller. Faultline found this in an earlier version of this
 * file, where a hung sensor made the control loop 77 ms late.
 *
 * If no good reading arrives for 50 ms (or readings look stuck), the loop
 * holds the last speed for up to 200 ms more, then stops the motor and raises
 * an alarm. It also measures how late
 * each tick starts, which is how Faultline shows whether the deadline was kept.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "fl.h"

#define TARGET_MLH   120.0
#define RPM_PER_MLH  0.5      /* feed-forward: 120 mL/h needs about 60 rpm */
#define KP           0.10
#define KI           0.60
#define STUCK_READS  5        /* 5 identical readings in a row = 50 ms */
#define LOSS_NS      50000000ULL   /* no good reading for 50 ms = sensor lost */
#define FRESH_NS     15000000ULL   /* a reading older than 15 ms is not used */
#define WORKER_PRIO  39

static struct fl_state *s;
static uint64_t timeout_ns = FL_SENSOR_TIMEOUT_NS;

static sem_t sensor_go, motor_go;
static volatile int sensor_busy, motor_busy;
static volatile uint64_t reading_ns;
static volatile double reading_mlh;
static volatile uint32_t reading_seq;
static volatile double motor_out;

static int read_flow(int *coid, struct fl_flow_reply *out)
{
    if (*coid == -1) {
        *coid = name_open(FL_FLOW_NAME, 0);
        if (*coid == -1) {
            return -1;
        }
    }
    struct fl_flow_req req = { FL_MSG_FLOW_READ, 0 };
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns, NULL);
    if (MsgSend(*coid, &req, sizeof(req), out, sizeof(*out)) == -1) {
        if (errno != ETIMEDOUT) {
            name_close(*coid);   /* server gone: reconnect next time */
            *coid = -1;
        }
        return -1;
    }
    return 0;
}

static void *sensor_worker(void *arg)
{
    (void)arg;
    int coid = -1;
    for (;;) {
        sem_wait(&sensor_go);
        struct fl_flow_reply r;
        if (read_flow(&coid, &r) == 0) {
            reading_mlh = r.flow_mlh;
            reading_seq = r.seq;
            __atomic_store_n(&reading_ns, fl_now_ns(), __ATOMIC_RELEASE);
        }
        __atomic_store_n(&sensor_busy, 0, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void *motor_worker(void *arg)
{
    (void)arg;
    int fd = -1;
    for (;;) {
        sem_wait(&motor_go);
        if (fd == -1) {
            fd = open(FL_MOTOR_PATH, O_WRONLY);
        }
        if (fd != -1) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%.3f", motor_out);
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns, NULL);
            if (write(fd, buf, n) == -1 && errno != ETIMEDOUT) {
                close(fd);   /* driver gone: reopen next time */
                fd = -1;
            }
        }
        __atomic_store_n(&motor_busy, 0, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void start_worker(void *(*fn)(void *))
{
    pthread_attr_t ta;
    pthread_attr_init(&ta);
    struct sched_param sp = { .sched_priority = WORKER_PRIO };
    pthread_attr_setinheritsched(&ta, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&ta, SCHED_FIFO);
    pthread_attr_setschedparam(&ta, &sp);
    pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &ta, fn, NULL);
}

/* Wakes a worker unless it is still busy with the previous job. */
static void kick(volatile int *busy, sem_t *go)
{
    int expected = 0;
    if (__atomic_compare_exchange_n(busy, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        sem_post(go);
    }
}

int main(void)
{
    s = fl_map(0);
    if (!s) {
        return 1;
    }
    fl_set_fifo(40);
    int me = fl_my_index();

    sem_init(&sensor_go, 0, 0);
    sem_init(&motor_go, 0, 0);
    start_worker(sensor_worker);
    start_worker(motor_worker);

    int chid = ChannelCreate(0);
    int self = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    timer_t tid;
    if (chid == -1 || self == -1 || fl_periodic_pulse(self, FL_PULSE_TICK, FL_CTRL_PERIOD_NS, &tid) == -1) {
        perror("pump-ctrl setup");
        return 1;
    }

    s->target_mlh = TARGET_MLH;
    int hb = -1;
    fl_heartbeat(&hb, me);   /* ready: loop about to start */
    double integral = 0, cmd = TARGET_MLH * RPM_PER_MLH;
    double prev_value = -1;
    uint32_t used_seq = 0;
    int same_count = 0;
    int was_late = 0;
    uint64_t start = fl_now_ns();
    uint64_t last = start;
    uint64_t tick = 0;
    if (s->sensor_ok_ns == 0) {
        s->sensor_ok_ns = start;
    }

    struct _pulse p;
    for (;;) {
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1 || p.code != FL_PULSE_TICK) {
            continue;
        }
        uint64_t now = fl_now_ns();
        tick++;

        /* How late did this tick start? */
        int64_t late = (int64_t)(now - (start + tick * FL_CTRL_PERIOD_NS));
        if (late < 0) {
            late = 0;
        }
        s->ctrl_last_late_ns = late;
        if (late > s->ctrl_max_late_ns) {
            s->ctrl_max_late_ns = late;
        }
        if (late > FL_LATE_LIMIT_NS) {
            s->ctrl_late++;
            if (!was_late) {
                fl_event(s, FL_EV_DEADLINE_MISS, me, getpid(), (int)(late / 1000),
                         "control tick %.1f ms late", late / 1e6);
            }
            was_late = 1;
        } else {
            was_late = 0;
        }
        if (late > 5 * FL_CTRL_PERIOD_NS) {
            start = now - tick * FL_CTRL_PERIOD_NS;   /* resync after a long stall */
        }
        double dt = (double)(now - last) / 1e9;
        last = now;

        /* Use the newest reading if it is fresh and new, then ask for the next one. */
        uint64_t rt = __atomic_load_n(&reading_ns, __ATOMIC_ACQUIRE);
        uint32_t seq = reading_seq;
        double value = reading_mlh;
        int ok = rt != 0 && now - rt < FRESH_NS && seq != used_seq;
        kick(&sensor_busy, &sensor_go);
        if (ok) {
            used_seq = seq;
            same_count = (value == prev_value) ? same_count + 1 : 0;
            prev_value = value;
            if (same_count >= STUCK_READS) {
                ok = 0;   /* plausibility check: a live sensor is never perfectly constant */
            }
        }
        if (!ok) {
            s->sensor_fail++;
        }

        int mode = s->pump_mode;
        if (!ok && rt == 0) {
            /* Just started: no reading yet. Run on feed-forward without raising anything. */
        } else if (ok) {
            s->sensor_ok_ns = now;
            s->last_flow_mlh = value;
            double err = TARGET_MLH - value;
            integral += err * dt;
            if (integral > 200) integral = 200;
            if (integral < -200) integral = -200;
            cmd = TARGET_MLH * RPM_PER_MLH + KP * err + KI * integral;
            if (mode != FL_PUMP_RUN) {
                fl_event(s, FL_EV_PUMP_RESUME, me, getpid(), 0, "sensor back, pump running");
                s->pump_mode = FL_PUMP_RUN;
            }
        } else if (now - s->sensor_ok_ns < LOSS_NS) {
            /* Brief gap: keep the last speed quietly. */
        } else if (now - s->sensor_ok_ns < LOSS_NS + FL_HOLD_GRACE_NS) {
            if (mode == FL_PUMP_RUN) {
                fl_event(s, FL_EV_PUMP_HOLD, me, getpid(), 0, "sensor lost, holding last speed");
                s->pump_mode = FL_PUMP_HOLD;
            }
        } else {
            cmd = 0;
            integral = 0;
            if (mode != FL_PUMP_ALARM) {
                fl_event(s, FL_EV_PUMP_ALARM, me, getpid(), 0, "no sensor for 200 ms: motor stopped, alarm");
                s->pump_mode = FL_PUMP_ALARM;
            }
        }

        motor_out = cmd;
        kick(&motor_busy, &motor_go);
        s->expected_ml += TARGET_MLH * dt / 3600.0;
        s->ctrl_ticks++;
        if (tick % 2 == 0) {
            fl_heartbeat(&hb, me);
        }
    }
}
