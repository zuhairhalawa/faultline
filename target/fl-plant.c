/*
 * fl-plant: the physical side of the pump, simulated at 1 kHz.
 *
 * The motor turns toward the speed its driver last commanded, with inertia.
 * Flow follows motor speed, and delivered volume is the integral of flow.
 * Faultline never faults this process: real hardware keeps obeying its last
 * command when the software driving it crashes.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/neutrino.h>
#include "fl.h"

#define PLANT_PERIOD_NS   1000000ULL   /* 1 kHz */
#define MOTOR_TAU_S       0.08         /* motor time constant */
#define ML_H_PER_RPM      2.0          /* 60 rpm gives 120 mL/h */

int main(void)
{
    struct fl_state *s = fl_map(0);
    if (!s) {
        return 1;
    }
    fl_set_fifo(60);
    int me = fl_my_index();

    int chid = ChannelCreate(0);
    int self = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    timer_t tid;
    if (chid == -1 || self == -1 || fl_periodic_pulse(self, FL_PULSE_TICK, PLANT_PERIOD_NS, &tid) == -1) {
        perror("plant setup");
        return 1;
    }

    int hb = -1;
    fl_heartbeat(&hb, me);   /* ready */
    uint64_t last = fl_now_ns();
    uint32_t n = 0;
    struct _pulse p;
    for (;;) {
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1) {
            continue;
        }
        uint64_t now = fl_now_ns();
        double dt = (double)(now - last) / 1e9;
        last = now;
        if (dt > 0.05) {
            dt = 0.05;   /* never integrate a huge step after a stall */
        }
        double cmd = s->motor_cmd_rpm;
        double rpm = s->motor_rpm + (cmd - s->motor_rpm) * (1.0 - exp(-dt / MOTOR_TAU_S));
        if (rpm < 0) {
            rpm = 0;
        }
        double flow = rpm * ML_H_PER_RPM;
        s->motor_rpm = rpm;
        s->flow_mlh = flow;
        s->volume_ml += flow * dt / 3600.0;
        s->plant_tick++;
        if (++n % 20 == 0) {
            fl_heartbeat(&hb, me);
        }
    }
}
