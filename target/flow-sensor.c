/*
 * flow-sensor: answers flow readings over QNX message passing.
 *
 * Registers the name "fl-flow". The controller connects with name_open() and
 * asks for a reading with MsgSend(). Two faults act here: slow replies, and a
 * stuck value that repeats the last reading.
 *
 * name_attach() gives the channel _NTO_CHF_UNBLOCK. When a client's
 * TimerTimeout expires while it waits for our reply, the kernel doesn't release
 * it; it sends us _PULSE_CODE_UNBLOCK and leaves the decision to us. So the
 * sensor never sleeps while holding a request. A slow reply is a deferred
 * reply: we keep the rcvid, arm a one-shot timer, and stay in MsgReceive(). If
 * the unblock pulse for that rcvid arrives first, we answer it with ETIMEDOUT,
 * and the client's timeout works. Only a frozen sensor can't do this, which is
 * why the controller does its reads from a worker thread.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "fl.h"

#define SENSOR_HEARTBEAT_NS 20000000ULL  /* 50 Hz */

typedef union {
    uint16_t type;
    struct _pulse pulse;
    struct fl_flow_req req;
} sensor_msg_t;

/* The one request we are holding back, if any (the controller sends one at a time). */
static rcvid_t held = 0;
static struct fl_flow_reply held_reply;

int main(void)
{
    struct fl_state *s = fl_map(0);
    if (!s) {
        return 1;
    }
    fl_set_fifo(42);
    int me = fl_my_index();

    name_attach_t *att = name_attach(NULL, FL_FLOW_NAME, 0);
    if (!att) {
        perror("name_attach " FL_FLOW_NAME);
        return 1;
    }
    int self = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);
    timer_t tid, reply_timer;
    struct sigevent reply_ev;
    SIGEV_PULSE_INIT(&reply_ev, self, SIGEV_PULSE_PRIO_INHERIT, FL_PULSE_REPLY, 0);
    if (self == -1 || fl_periodic_pulse(self, FL_PULSE_TICK, SENSOR_HEARTBEAT_NS, &tid) == -1 ||
        timer_create(CLOCK_MONOTONIC, &reply_ev, &reply_timer) == -1) {
        perror("flow-sensor setup");
        return 1;
    }

    int hb = -1;
    fl_heartbeat(&hb, me);   /* ready: name attached */
    uint32_t seq = 0;
    double last_value = 0;
    srand((unsigned)fl_now_ns());
    sensor_msg_t msg;
    for (;;) {
        rcvid_t rcvid = MsgReceive(att->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            continue;
        }
        if (rcvid == 0) {
            switch (msg.pulse.code) {
            case FL_PULSE_TICK:
                fl_heartbeat(&hb, me);
                break;
            case FL_PULSE_REPLY:
                if (held) {
                    MsgReply(held, EOK, &held_reply, sizeof(held_reply));   /* the delayed reading */
                    held = 0;
                }
                break;
            case _PULSE_CODE_UNBLOCK:
                /* The client gave up (its TimerTimeout expired). Release it now. */
                if (held && (rcvid_t)msg.pulse.value.sival_long == held) {
                    struct itimerspec off = { 0 };
                    timer_settime(reply_timer, 0, &off, NULL);
                    MsgError(held, ETIMEDOUT);
                    held = 0;
                }
                break;
            case _PULSE_CODE_DISCONNECT:
                ConnectDetach(msg.pulse.scoid);
                break;
            default:
                break;
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) {
            MsgReply(rcvid, EOK, NULL, 0);   /* name_open() handshake */
            continue;
        }
        if (msg.type != FL_MSG_FLOW_READ) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        uint64_t now = fl_now_ns();
        struct fl_flow_reply r = { 0 };
        if (now < s->stuck_until_ns) {
            r.flow_mlh = last_value;   /* a stuck sensor keeps reporting the same value */
            r.stuck = 1;
        } else {
            double noise = ((double)rand() / RAND_MAX - 0.5) * 0.01;   /* +/- 0.5% */
            r.flow_mlh = s->flow_mlh * (1.0 + noise);
            last_value = r.flow_mlh;
        }
        r.sample_ns = now;
        r.seq = ++seq;

        if (now < s->slow_until_ns && s->slow_reply_ms > 0 && !held) {
            /* Slow reply fault: hold the request and answer it when the timer fires. */
            held = rcvid;
            held_reply = r;
            struct itimerspec its = { 0 };
            its.it_value.tv_sec = s->slow_reply_ms / 1000;
            its.it_value.tv_nsec = (s->slow_reply_ms % 1000) * 1000000L;
            timer_settime(reply_timer, 0, &its, NULL);
            continue;
        }
        MsgReply(rcvid, EOK, &r, sizeof(r));
    }
}
