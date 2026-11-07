/*
 * Faultline: shared definitions for every process on the QNX target.
 *
 * All processes map one shared-memory object, /fl-state. It holds the simulated
 * plant (the physical pump), controller status, the process table the supervisor
 * keeps, active fault settings, and an event log the dashboard reads.
 */
#ifndef FL_H
#define FL_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/neutrino.h>

#define FL_SHM_NAME      "/fl-state"
#define FL_MAGIC         0x464c5432u /* "FLT2" */
#define FL_SUPERVISOR    "fl-supervisor"
#define FL_FLOW_NAME     "fl-flow"
#define FL_MOTOR_PATH    "/dev/fl/motor"
#define FL_INJECT_PATH   "/dev/faultline/inject"

#define FL_MAXPROC       8
#define FL_EVENTS        512

/* Pulse codes sent to the supervisor's channel. */
#define FL_PULSE_HEARTBEAT (_PULSE_CODE_MINAVAIL + 1)
#define FL_PULSE_TICK      (_PULSE_CODE_MINAVAIL + 2)
#define FL_PULSE_CHILD     (_PULSE_CODE_MINAVAIL + 3)

/* Message the controller sends to the flow sensor. Kept outside the _IO_* range. */
#define FL_MSG_FLOW_READ 0x4601
struct fl_flow_req {
    uint16_t type;
    uint16_t subtype;
};
struct fl_flow_reply {
    double   flow_mlh;
    uint64_t sample_ns;
    uint32_t seq;
    uint32_t stuck;
};

enum fl_proc_state {
    FL_DOWN = 0,
    FL_STARTING,
    FL_RUNNING,
    FL_RESTARTING,
    FL_FAILED
};

enum fl_pump_mode {
    FL_PUMP_RUN = 0,
    FL_PUMP_HOLD,     /* sensor lost: holding last speed, within the grace period */
    FL_PUMP_ALARM     /* safe state: motor stopped, alarm raised */
};

enum fl_event_type {
    FL_EV_START = 1,      /* process started (first launch) */
    FL_EV_INJECT,         /* a fault was injected */
    FL_EV_DEATH,          /* supervisor saw a process die */
    FL_EV_HANG,           /* supervisor saw a missed heartbeat deadline */
    FL_EV_RESTART,        /* supervisor started a replacement */
    FL_EV_READY,          /* replacement sent its first heartbeat */
    FL_EV_DEADLINE_MISS,  /* control loop ran late beyond its limit */
    FL_EV_PUMP_HOLD,
    FL_EV_PUMP_ALARM,
    FL_EV_PUMP_RESUME,
    FL_EV_FAULT_END,      /* a timed fault (flood, slow, stuck) finished */
    FL_EV_GAVE_UP         /* restart limit reached */
};

struct fl_proc {
    char     name[16];
    char     path[64];
    pid_t    pid;
    int      prio;
    int      heartbeat_ms;   /* 0 = not watched for hangs */
    int      restart_limit;
    int      faultable;      /* 0 = the injector refuses to fault it */
    volatile int state;
    volatile uint64_t last_hb_ns;
    volatile uint64_t started_ns;
    volatile uint64_t fault_ns;      /* when the last fault on it was injected, 0 if none pending */
    volatile uint64_t detect_ns;     /* when the supervisor noticed the failure */
    volatile uint32_t restarts;
    volatile int last_cause;         /* FL_EV_DEATH or FL_EV_HANG */
};

struct fl_event {
    uint64_t t_ns;
    int32_t  type;
    int32_t  proc;      /* index into proc[], or -1 */
    int32_t  pid;
    int32_t  arg;       /* event specific: signal number, latency in microseconds, ... */
    char     text[56];
};

struct fl_state {
    uint32_t magic;
    uint64_t boot_ns;

    /* Plant: the physical pump. Written only by fl-plant, except motor_cmd. */
    volatile double   motor_cmd_rpm;   /* written by motor-drv */
    volatile double   motor_rpm;
    volatile double   flow_mlh;
    volatile double   volume_ml;
    volatile uint64_t plant_tick;

    /* Controller status, written by pump-ctrl. */
    volatile double   target_mlh;
    volatile double   expected_ml;
    volatile double   last_flow_mlh;
    volatile int      pump_mode;
    volatile uint64_t ctrl_ticks;
    volatile uint64_t ctrl_late;        /* ticks later than FL_LATE_LIMIT_NS */
    volatile int64_t  ctrl_last_late_ns;
    volatile int64_t  ctrl_max_late_ns;
    volatile uint64_t sensor_ok_ns;     /* time of last good sensor reading */
    volatile uint64_t sensor_fail;      /* failed reads */

    /* Fault settings, written by fl-inject, read by the faulted process. */
    volatile int      slow_reply_ms;
    volatile uint64_t slow_until_ns;
    volatile uint64_t stuck_until_ns;

    int nproc;
    struct fl_proc proc[FL_MAXPROC];

    volatile uint32_t ev_head;          /* total events written; slot = head % FL_EVENTS */
    struct fl_event ev[FL_EVENTS];
};

/* Control loop timing. */
#define FL_CTRL_PERIOD_NS   10000000LL  /* 100 Hz */
#define FL_LATE_LIMIT_NS     2000000LL  /* a tick more than 2 ms late counts as a miss */
#define FL_SENSOR_TIMEOUT_NS 5000000ULL /* 5 ms to answer a read */
#define FL_HOLD_GRACE_NS   200000000ULL /* hold last speed for 200 ms, then stop */

/* Helpers in fl.c */
uint64_t fl_now_ns(void);
struct fl_state *fl_map(int create);
void fl_event(struct fl_state *s, int type, int proc, int pid, int arg, const char *fmt, ...);
int  fl_find_proc(struct fl_state *s, const char *name);
int  fl_my_index(void);                 /* from FL_PROC in the environment, -1 if unset */
int  fl_heartbeat_open(void);           /* connection to the supervisor, -1 on failure */
void fl_heartbeat(int *coid, int index);
void fl_set_fifo(int prio);
int  fl_periodic_pulse(int coid, int code, uint64_t period_ns, timer_t *out);

#endif
