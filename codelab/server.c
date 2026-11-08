/*
 * server: answers ping requests over QNX message passing.
 *
 *   server [-d DELAY_MS] [-u]
 *
 *   -d DELAY_MS  answer each request DELAY_MS late (default: at once)
 *   -u           the fixed server: keep a slow request as a deferred reply,
 *                and release the client when its timeout fires
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

int main(int argc, char **argv)
{
    int delay_ms = 0, fixed = 0, opt;
    while ((opt = getopt(argc, argv, "d:u")) != -1) {
        if (opt == 'd') delay_ms = atoi(optarg);
        else if (opt == 'u') fixed = 1;
        else { fprintf(stderr, "usage: server [-d DELAY_MS] [-u]\n"); return 2; }
    }

    name_attach_t *att = name_attach(NULL, PING_NAME, 0);
    if (!att) { perror("name_attach"); return 1; }
    printf("server: pid %d, name %s, delay %d ms, %s\n", getpid(), PING_NAME, delay_ms,
           fixed ? "fixed (deferred replies, honours unblock)" : "naive (sleeps, then replies)");
    fflush(stdout);

    /* For the fixed server: a one-shot timer that tells us when a held reply is due. */
    int self = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self, SIGEV_PULSE_PRIO_INHERIT, PULSE_REPLY_DUE, 0);
    timer_t reply_timer;
    timer_create(CLOCK_MONOTONIC, &ev, &reply_timer);
    rcvid_t held = 0;                 /* the request we are holding, if any */
    struct ping_reply held_reply;

    server_msg_t msg;
    for (;;) {
        rcvid_t rcvid = MsgReceive(att->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) continue;

        if (rcvid == 0) {             /* a pulse */
            switch (msg.pulse.code) {
            case _PULSE_CODE_UNBLOCK: {
                rcvid_t who = (rcvid_t)msg.pulse.value.sival_long;
                if (fixed && who == held) {
                    struct itimerspec off = { 0 };
                    timer_settime(reply_timer, 0, &off, NULL);
                    MsgError(held, ETIMEDOUT);    /* this is what actually frees the client */
                    held = 0;
                    printf("server: client gave up waiting; released it with ETIMEDOUT\n");
                } else if (!fixed) {
                    printf("server: got _PULSE_CODE_UNBLOCK (ignored; already replied)\n");
                }
                fflush(stdout);
                break;
            }
            case PULSE_REPLY_DUE:
                if (held) { MsgReply(held, EOK, &held_reply, sizeof(held_reply)); held = 0; }
                break;
            case _PULSE_CODE_DISCONNECT:
                ConnectDetach(msg.pulse.scoid);
                break;
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }   /* name_open() */
        if (msg.type != PING_TYPE)   { MsgError(rcvid, ENOSYS); continue; }

        struct ping_reply r = { msg.req.seq, (uint32_t)getpid() };
        if (delay_ms == 0) {
            MsgReply(rcvid, EOK, &r, sizeof(r));
        } else if (!fixed) {
            /* Naive: sleep while holding the request. Unblock pulses wait in the queue. */
            struct timespec d = { delay_ms / 1000, (delay_ms % 1000) * 1000000L };
            nanosleep(&d, NULL);
            MsgReply(rcvid, EOK, &r, sizeof(r));
        } else if (held) {
            MsgError(rcvid, EBUSY);    /* this small server holds one request at a time */
        } else {
            /* Fixed: remember the request, arm the timer, and go straight back to MsgReceive. */
            held = rcvid;
            held_reply = r;
            struct itimerspec its = { 0 };
            its.it_value.tv_sec = delay_ms / 1000;          /* one-shot: it_interval stays 0 */
            its.it_value.tv_nsec = (delay_ms % 1000) * 1000000L;
            timer_settime(reply_timer, 0, &its, NULL);
        }
    }
}
