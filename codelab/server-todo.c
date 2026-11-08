/*
 * cl-server-todo: the exercise version of cl-server. You fill in the fix.
 *
 *   cl-server-todo [-d DELAY_MS] [-u]
 *
 * It behaves exactly like cl-server, except that -u does nothing useful yet:
 * a slow request is still answered by sleeping, and unblock pulses are ignored.
 * Complete TODO 1 and TODO 2 below, rebuild with `make`, and the client's
 * timeout will work. The finished version is server.c.
 *
 * name_attach() creates the channel with _NTO_CHF_UNBLOCK. That flag means:
 * when a client that is waiting for our reply wants to stop waiting (its
 * TimerTimeout fired, or it got a signal), the kernel does not release it.
 * It sends us a _PULSE_CODE_UNBLOCK pulse and lets us decide.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "ping.h"

#define PULSE_REPLY_DUE (_PULSE_CODE_MINAVAIL + 1)

typedef union {
    uint16_t type;
    struct _pulse pulse;
    struct ping_req req;
} server_msg_t;

static void die(const char *what)
{
    perror(what);
    exit(1);
}

int main(int argc, char **argv)
{
    int delay_ms = 0, fixed = 0, opt;
    while ((opt = getopt(argc, argv, "d:u")) != -1) {
        if (opt == 'd') delay_ms = atoi(optarg);
        else if (opt == 'u') fixed = 1;
        else { fprintf(stderr, "usage: cl-server-todo [-d DELAY_MS] [-u]\n"); return 2; }
    }

    name_attach_t *att = name_attach(NULL, PING_NAME, 0);
    if (!att) die("name_attach");
    printf("server: pid %d, name %s, delay %d ms, %s\n", getpid(), PING_NAME, delay_ms,
           fixed ? "exercise (-u runs your TODO 1 and TODO 2 code)" : "naive (sleeps, then replies)");
    fflush(stdout);

    /* For the fixed server: a one-shot timer that tells us when a held reply is due. */
    int self = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);
    if (self == -1) die("ConnectAttach");
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self, SIGEV_PULSE_PRIO_INHERIT, PULSE_REPLY_DUE, 0);
    timer_t reply_timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &reply_timer) == -1) die("timer_create");
    struct itimerspec off = { 0 };
    rcvid_t held = 0;                 /* the request we are holding, if any */
    struct ping_reply held_reply;

    server_msg_t msg;
    struct _msg_info info;
    for (;;) {
        rcvid_t rcvid = MsgReceive(att->chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) {
            if (errno == EINTR) continue;
            die("MsgReceive");
        }

        if (rcvid == 0) {             /* a pulse */
            switch (msg.pulse.code) {
            case _PULSE_CODE_UNBLOCK: {
                /* The value is the rcvid of the client that wants to stop waiting. */
                rcvid_t who = (rcvid_t)msg.pulse.value.sival_long;
                if (fixed && who == held) {
                    /* TODO 1: the client we are holding wants to stop waiting.
                     * Release it now with an error it will recognise as a timeout, and make
                     * sure the reply you scheduled in TODO 2 doesn't go out later.
                     * (`off` is an all-zero itimerspec you may find useful.)
                     */
                    (void)off;
                    printf("server: client gave up waiting (TODO 1 decides what happens next)\n");
                } else if (!fixed) {
                    printf("server: got _PULSE_CODE_UNBLOCK (ignored; already replied)\n");
                }
                fflush(stdout);
                break;
            }
            case PULSE_REPLY_DUE:
                if (held) {
                    if (MsgReply(held, EOK, &held_reply, sizeof(held_reply)) == -1) perror("MsgReply");
                    held = 0;
                }
                break;
            case _PULSE_CODE_DISCONNECT:
                ConnectDetach(msg.pulse.scoid);
                break;
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }   /* name_open() */
        if (msg.type != PING_TYPE || info.msglen < (ssize_t)sizeof(struct ping_req)) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        struct ping_reply r = { msg.req.seq, (uint32_t)getpid() };
        if (delay_ms == 0) {
            MsgReply(rcvid, EOK, &r, sizeof(r));
        } else if (!fixed) {
            /* Naive: sleep while holding the request. Unblock pulses wait in the queue. */
            struct timespec d = { 0 };
            d.tv_sec = delay_ms / 1000;
            d.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&d, NULL);
            MsgReply(rcvid, EOK, &r, sizeof(r));
        } else if (held) {
            MsgError(rcvid, EBUSY);    /* this small server holds one request at a time */
        } else {
            /* TODO 2: don't sleep here. Keep this request as a deferred reply, and arrange
             * for reply_timer to deliver PULSE_REPLY_DUE once, delay_ms from now. Then let
             * the loop go back to MsgReceive(); the PULSE_REPLY_DUE case above already
             * sends the held reply. Replace the four lines below.
             */
            struct timespec d = { 0 };
            d.tv_sec = delay_ms / 1000;
            d.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&d, NULL);
            MsgReply(rcvid, EOK, &r, sizeof(r));
        }
    }
}
