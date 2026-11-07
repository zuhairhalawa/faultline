/*
 * flow-sensor: answers flow readings over QNX message passing.
 *
 * Registers the name "fl-flow". The controller connects with name_open() and
 * asks for a reading with MsgSend(). Two faults act here: slow replies, and a
 * stuck value that repeats the last reading.
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
    timer_t tid;
    if (self == -1 || fl_periodic_pulse(self, FL_PULSE_TICK, SENSOR_HEARTBEAT_NS, &tid) == -1) {
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
            if (msg.pulse.code == FL_PULSE_TICK) {
                fl_heartbeat(&hb, me);
            } else if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(msg.pulse.scoid);
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

        if (now < s->slow_until_ns && s->slow_reply_ms > 0) {
            struct timespec d = { s->slow_reply_ms / 1000, (s->slow_reply_ms % 1000) * 1000000L };
            nanosleep(&d, NULL);
        }
        MsgReply(rcvid, EOK, &r, sizeof(r));
    }
}
