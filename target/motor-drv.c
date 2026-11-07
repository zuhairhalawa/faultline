/*
 * motor-drv: the pump motor driver, as a QNX resource manager.
 *
 * It owns the path /dev/fl/motor. Writing a number sets the motor speed in rpm,
 * so the controller (or a person at the shell) drives it with write() or echo.
 * Reading returns the commanded and actual speed. A crash here is the headline
 * demo: the supervisor restarts the driver while the motor keeps its last command.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include "fl.h"

#define MAX_RPM 120.0
#define DRV_HEARTBEAT_NS 20000000ULL

static struct fl_state *s;
static int me = -1;
static int hb = -1;

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
    char buf[64];
    size_t n = nbytes < sizeof(buf) - 1 ? nbytes : sizeof(buf) - 1;
    if (resmgr_msgget(ctp, buf, n, sizeof(msg->i)) == -1) {
        return errno;
    }
    buf[n] = '\0';
    char *end;
    double rpm = strtod(buf, &end);
    if (end == buf) {
        return EINVAL;
    }
    if (rpm < 0) {
        rpm = 0;
    }
    if (rpm > MAX_RPM) {
        rpm = MAX_RPM;
    }
    s->motor_cmd_rpm = rpm;
    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    return EOK;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    static char buf[96];   /* the reply is sent after we return, so not on the stack */
    int len = snprintf(buf, sizeof(buf), "cmd_rpm=%.2f rpm=%.2f\n", s->motor_cmd_rpm, s->motor_rpm);
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
    return 0;
}

int main(void)
{
    s = fl_map(0);
    if (!s) {
        return 1;
    }
    fl_set_fifo(45);
    me = fl_my_index();

    dispatch_t *dpp = dispatch_create();
    if (!dpp) {
        perror("dispatch_create");
        return 1;
    }
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
    if (resmgr_attach(dpp, &rattr, FL_MOTOR_PATH, _FTYPE_ANY, 0, &cfuncs, &iofuncs, &attr) == -1) {
        perror("resmgr_attach " FL_MOTOR_PATH);
        return 1;
    }

    int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
    int self = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    timer_t tid;
    if (code == -1 || self == -1 || fl_periodic_pulse(self, code, DRV_HEARTBEAT_NS, &tid) == -1) {
        perror("motor-drv heartbeat timer");
        return 1;
    }

    fl_heartbeat(&hb, me);   /* ready: /dev/fl/motor exists */
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        ctp = dispatch_block(ctp);
        if (ctp) {
            dispatch_handler(ctp);
        }
    }
}
