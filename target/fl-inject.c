/*
 * fl-inject: injects faults on command, as a QNX resource manager.
 *
 * It owns /dev/faultline/inject. Each write is one command:
 *   crash <name>             SIGSEGV to the process
 *   hang <name>              SIGSTOP: alive but frozen
 *   flood <prio> <ms>        one busy thread per CPU at SCHED_FIFO <prio> for <ms>
 *   slow <ms> <for_ms>       the flow sensor delays each reply by <ms>
 *   stuck <for_ms>           the flow sensor repeats its last value
 * Reading the file lists the commands and the processes that can be faulted.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include "fl.h"

#define INJ_HEARTBEAT_NS 50000000ULL

static struct fl_state *s;
static int me = -1;
static int hb = -1;

struct flood_arg {
    int prio;
    int ms;
    int last;   /* only one thread logs the end */
};

static void *flood_thread(void *arg)
{
    struct flood_arg a = *(struct flood_arg *)arg;
    free(arg);
    fl_set_fifo(a.prio);
    uint64_t end = fl_now_ns() + (uint64_t)a.ms * 1000000ULL;
    volatile uint64_t spin = 0;
    while (fl_now_ns() < end) {
        spin++;
    }
    if (a.last) {
        fl_event(s, FL_EV_FAULT_END, -1, getpid(), a.prio, "CPU flood at priority %d ended", a.prio);
    }
    return NULL;
}

static int target_index(const char *name, int *err)
{
    int i = fl_find_proc(s, name);
    if (i < 0) {
        *err = ENOENT;
        return -1;
    }
    if (!s->proc[i].faultable) {
        *err = EPERM;
        return -1;
    }
    if (s->proc[i].state != FL_RUNNING) {
        *err = EBUSY;   /* already down or restarting */
        return -1;
    }
    return i;
}

static int run_command(char *cmd)
{
    char verb[16] = "", a1[32] = "", a2[32] = "";
    if (sscanf(cmd, "%15s %31s %31s", verb, a1, a2) < 1) {
        return EINVAL;
    }
    int err = EOK;
    uint64_t now = fl_now_ns();

    if (strcmp(verb, "crash") == 0 || strcmp(verb, "hang") == 0) {
        int i = target_index(a1, &err);
        if (i < 0) {
            return err;
        }
        int sig = verb[0] == 'c' ? SIGSEGV : SIGSTOP;
        s->proc[i].fault_ns = now;
        fl_event(s, FL_EV_INJECT, i, s->proc[i].pid, sig, "inject %s %s", verb, a1);
        if (kill(s->proc[i].pid, sig) == -1) {
            s->proc[i].fault_ns = 0;
            return errno;
        }
        return EOK;
    }
    if (strcmp(verb, "flood") == 0) {
        int prio = atoi(a1), ms = atoi(a2);
        if (prio < 1 || prio > 63 || ms < 1 || ms > 10000) {
            return EINVAL;
        }
        struct flood_arg *fa = malloc(sizeof(*fa));
        fa->prio = prio;
        fa->ms = ms;
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpu < 1) ncpu = 1;
        fl_event(s, FL_EV_INJECT, -1, getpid(), prio, "inject flood prio %d for %d ms on %ld CPUs", prio, ms, ncpu);
        pthread_attr_t ta;
        pthread_attr_init(&ta);
        pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
        for (long c = 0; c < ncpu; c++) {
            struct flood_arg *each = malloc(sizeof(*each));
            *each = *fa;
            each->last = (c == ncpu - 1);
            pthread_t t;
            int rc = pthread_create(&t, &ta, flood_thread, each);
            if (rc != EOK) {
                free(fa);
                return rc;
            }
        }
        free(fa);
        return EOK;
    }
    if (strcmp(verb, "slow") == 0) {
        int ms = atoi(a1), dur = atoi(a2);
        if (ms < 1 || ms > 1000 || dur < 1 || dur > 60000) {
            return EINVAL;
        }
        s->slow_reply_ms = ms;
        s->slow_until_ns = now + (uint64_t)dur * 1000000ULL;
        fl_event(s, FL_EV_INJECT, fl_find_proc(s, "flow-sensor"), 0, ms,
                 "inject slow sensor replies +%d ms for %d ms", ms, dur);
        return EOK;
    }
    if (strcmp(verb, "stuck") == 0) {
        int dur = atoi(a1);
        if (dur < 1 || dur > 60000) {
            return EINVAL;
        }
        s->stuck_until_ns = now + (uint64_t)dur * 1000000ULL;
        fl_event(s, FL_EV_INJECT, fl_find_proc(s, "flow-sensor"), 0, dur,
                 "inject stuck sensor for %d ms", dur);
        return EOK;
    }
    return EINVAL;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    size_t nbytes = _IO_WRITE_GET_NBYTES(msg);
    char buf[128];
    size_t n = nbytes < sizeof(buf) - 1 ? nbytes : sizeof(buf) - 1;
    if (resmgr_msgget(ctp, buf, n, sizeof(msg->i)) == -1) {
        return errno;
    }
    buf[n] = '\0';
    int rc = run_command(buf);
    if (rc != EOK) {
        return rc;   /* the writer sees the error, e.g. "echo: write failed: No such file" */
    }
    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    return EOK;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    static char buf[1024];
    int len = snprintf(buf, sizeof(buf),
                       "commands: crash <name> | hang <name> | flood <prio> <ms> | slow <ms> <for_ms> | stuck <for_ms>\n"
                       "faultable:");
    for (int i = 0; i < s->nproc && len < (int)sizeof(buf) - 20; i++) {
        if (s->proc[i].faultable) {
            len += snprintf(buf + len, sizeof(buf) - len, " %s", s->proc[i].name);
        }
    }
    len += snprintf(buf + len, sizeof(buf) - len, "\n");
    iofunc_ocb_t *o = (iofunc_ocb_t *)ocb;
    off_t off = o->offset;
    if (off >= len) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    size_t want = _IO_READ_GET_NBYTES(msg);
    size_t give = (size_t)(len - off) < want ? (size_t)(len - off) : want;
    o->offset += give;
    _IO_SET_READ_NBYTES(ctp, give);
    SETIOV(ctp->iov, buf + off, give);
    return _RESMGR_NPARTS(1);
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)ctp; (void)code; (void)flags; (void)handle;
    fl_heartbeat(&hb, me);
    uint64_t now = fl_now_ns();
    static int slow_on, stuck_on;
    int slow = now < s->slow_until_ns, stuck = now < s->stuck_until_ns;
    if (slow_on && !slow) {
        fl_event(s, FL_EV_FAULT_END, -1, 0, 0, "slow sensor replies ended");
    }
    if (stuck_on && !stuck) {
        fl_event(s, FL_EV_FAULT_END, -1, 0, 0, "stuck sensor ended");
    }
    slow_on = slow;
    stuck_on = stuck;
    return 0;
}

int main(void)
{
    s = fl_map(0);
    if (!s) {
        return 1;
    }
    fl_set_fifo(50);
    me = fl_my_index();

    dispatch_t *dpp = dispatch_create();
    static resmgr_connect_funcs_t cfuncs;
    static resmgr_io_funcs_t iofuncs;
    static iofunc_attr_t attr;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs, _RESMGR_IO_NFUNCS, &iofuncs);
    iofuncs.write = io_write;
    iofuncs.read = io_read;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    if (!dpp || resmgr_attach(dpp, &rattr, FL_INJECT_PATH, _FTYPE_ANY, 0, &cfuncs, &iofuncs, &attr) == -1) {
        perror("resmgr_attach " FL_INJECT_PATH);
        return 1;
    }
    int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
    int self = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    timer_t tid;
    if (code == -1 || self == -1 || fl_periodic_pulse(self, code, INJ_HEARTBEAT_NS, &tid) == -1) {
        return 1;
    }
    fl_heartbeat(&hb, me);   /* ready */
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        ctp = dispatch_block(ctp);
        if (ctp) {
            dispatch_handler(ctp);
        }
    }
}
