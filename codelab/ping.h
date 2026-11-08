/* ping.h: the message the codelab's client and server exchange. */
#ifndef PING_H
#define PING_H

#include <stdint.h>
#include <time.h>
#include <sys/neutrino.h>

#define PING_NAME   "cl-ping"   /* registered under /dev/name/local/cl-ping */
#define PING_TYPE   0x5001      /* kept above the _IO_* message range */

struct ping_req {
    uint16_t type;
    uint16_t subtype;
    uint32_t seq;
};

struct ping_reply {
    uint32_t seq;
    uint32_t server_pid;
};

static inline double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#endif
