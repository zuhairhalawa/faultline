/*
 * fl-bridge: links the target to the dashboard over TCP (port 7700).
 *
 * Every 50 ms it sends one JSON line with the current state, then one line per
 * new event. Lines it receives are fault commands, passed straight to
 * /dev/faultline/inject, and it answers each with an ack line.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include "fl.h"

#define PORT 7700
#define MAX_CLIENTS 4

struct client {
    int fd;
    uint32_t ev_next;
    uint64_t wait_ns;     /* when we started waiting on an unfinished event, 0 if not */
    char in[256];
    size_t in_len;
};

static struct fl_state *s;
static const char *state_names[] = { "down", "starting", "running", "restarting", "failed" };
static const char *mode_names[] = { "run", "hold", "alarm" };

static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < cap; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
        }
        dst[j++] = (c >= 32 && c < 127) ? c : ' ';
    }
    dst[j] = '\0';
}

static int send_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = send(fd, buf, len, 0);
        if (n <= 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* win_late_ns: the worst control tick since the previous state line. */
static int send_state(int fd, int64_t win_late_ns)
{
    char buf[4096];
    uint64_t now = fl_now_ns();
    int n = snprintf(buf, sizeof(buf),
        "{\"kind\":\"state\",\"t_ms\":%.3f,"
        "\"plant\":{\"cmd_rpm\":%.3f,\"rpm\":%.3f,\"flow_mlh\":%.3f,\"volume_ml\":%.5f},"
        "\"ctrl\":{\"target_mlh\":%.1f,\"expected_ml\":%.5f,\"flow_mlh\":%.3f,\"mode\":\"%s\","
        "\"ticks\":%llu,\"late\":%llu,\"last_late_us\":%lld,\"max_late_us\":%lld,\"win_late_us\":%lld,"
        "\"sensor_fail\":%llu},"
        "\"procs\":[",
        (now - s->boot_ns) / 1e6,
        s->motor_cmd_rpm, s->motor_rpm, s->flow_mlh, s->volume_ml,
        s->target_mlh, s->expected_ml, s->last_flow_mlh,
        mode_names[s->pump_mode >= 0 && s->pump_mode <= 2 ? s->pump_mode : 0],
        (unsigned long long)s->ctrl_ticks, (unsigned long long)s->ctrl_late,
        (long long)(s->ctrl_last_late_ns / 1000), (long long)(s->ctrl_max_late_ns / 1000),
        (long long)(win_late_ns / 1000), (unsigned long long)s->sensor_fail);
    for (int i = 0; i < s->nproc && n < (int)sizeof(buf) - 256; i++) {
        struct fl_proc *p = &s->proc[i];
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"name\":\"%s\",\"pid\":%d,\"prio\":%d,\"state\":\"%s\",\"restarts\":%u,"
            "\"heartbeat_ms\":%d,\"faultable\":%d,\"hb_age_ms\":%.1f}",
            i ? "," : "", p->name, (int)p->pid, p->prio,
            state_names[p->state >= 0 && p->state <= 4 ? p->state : 0], p->restarts,
            p->heartbeat_ms, p->faultable,
            p->last_hb_ns ? (now - p->last_hb_ns) / 1e6 : -1.0);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}\n");
    return send_all(fd, buf, (size_t)n);
}

static int send_events(struct client *c)
{
    uint32_t head = s->ev_head;
    if (head - c->ev_next > FL_EVENTS) {
        c->ev_next = head - FL_EVENTS;   /* fell behind: skip what was overwritten */
    }
    char buf[512], text[120];
    while (c->ev_next != head) {
        struct fl_event *e = &s->ev[c->ev_next % FL_EVENTS];
        uint32_t want = c->ev_next + 1;
        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != want) {
            if (e->seq == 0 || (int32_t)(e->seq - want) < 0) {
                /* Still being written: send it on a later pass. A writer killed mid-event
                 * (a crash fault can land there) never finishes, so give up after 100 ms. */
                uint64_t now = fl_now_ns();
                if (c->wait_ns == 0) {
                    c->wait_ns = now;
                } else if (now - c->wait_ns > 100000000ULL) {
                    c->wait_ns = 0;
                    c->ev_next++;
                    continue;
                }
                break;
            }
            c->wait_ns = 0;
            c->ev_next++;        /* already overwritten by a newer event */
            continue;
        }
        struct fl_event copy = *e;
        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != want) {
            c->ev_next++;        /* overwritten while we copied it */
            continue;
        }
        e = &copy;
        json_escape(text, sizeof(text), e->text);
        const char *pname = (e->proc >= 0 && e->proc < s->nproc) ? s->proc[e->proc].name : "";
        int n = snprintf(buf, sizeof(buf),
            "{\"kind\":\"event\",\"seq\":%u,\"t_ms\":%.3f,\"type\":%d,\"proc\":\"%s\",\"pid\":%d,\"arg\":%d,\"text\":\"%s\"}\n",
            c->ev_next, (e->t_ns - s->boot_ns) / 1e6, e->type, pname, e->pid, e->arg, text);
        if (send_all(c->fd, buf, (size_t)n) == -1) {
            return -1;
        }
        c->ev_next++;
    }
    return 0;
}

static int run_command(int fd, const char *cmd)
{
    int err = 0;
    int f = open(FL_INJECT_PATH, O_WRONLY);
    if (f == -1) {
        err = errno;
    } else {
        if (write(f, cmd, strlen(cmd)) == -1) {
            err = errno;
        }
        close(f);
    }
    char esc[200], buf[320];
    json_escape(esc, sizeof(esc), cmd);
    int n = snprintf(buf, sizeof(buf), "{\"kind\":\"ack\",\"cmd\":\"%s\",\"ok\":%s,\"error\":\"%s\"}\n",
                     esc, err ? "false" : "true", err ? strerror(err) : "");
    return send_all(fd, buf, (size_t)n);
}

static int read_client(struct client *c)
{
    ssize_t n = recv(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len, 0);
    if (n <= 0) {
        return -1;
    }
    c->in_len += (size_t)n;
    c->in[c->in_len] = '\0';
    char *nl;
    while ((nl = strchr(c->in, '\n')) != NULL) {
        *nl = '\0';
        if (nl > c->in && nl[-1] == '\r') {
            nl[-1] = '\0';
        }
        if (c->in[0] && run_command(c->fd, c->in) == -1) {
            return -1;
        }
        size_t rest = c->in_len - (size_t)(nl + 1 - c->in);
        memmove(c->in, nl + 1, rest);
        c->in_len = rest;
        c->in[rest] = '\0';
    }
    if (c->in_len >= sizeof(c->in) - 1) {
        c->in_len = 0;   /* drop an over-long line */
    }
    return 0;
}

int main(void)
{
    s = fl_map(0);
    if (!s) {
        return 1;
    }
    fl_set_fifo(20);
    int me = fl_my_index();
    int hb = -1;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls == -1 || bind(ls, (struct sockaddr *)&addr, sizeof(addr)) == -1 || listen(ls, 4) == -1) {
        perror("fl-bridge listen");
        return 1;
    }

    struct client cl[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        cl[i].fd = -1;
    }
    fl_heartbeat(&hb, me);   /* ready: listening */
    uint64_t next_state = fl_now_ns();
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        int maxfd = ls;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (cl[i].fd != -1) {
                FD_SET(cl[i].fd, &rf);
                if (cl[i].fd > maxfd) maxfd = cl[i].fd;
            }
        }
        struct timeval tv = { 0, 10000 };
        select(maxfd + 1, &rf, NULL, NULL, &tv);

        if (FD_ISSET(ls, &rf)) {
            int fd = accept(ls, NULL, NULL);
            int placed = 0;
            for (int i = 0; fd != -1 && i < MAX_CLIENTS; i++) {
                if (cl[i].fd == -1) {
                    cl[i].fd = fd;
                    cl[i].in_len = 0;
                    cl[i].wait_ns = 0;
                    uint32_t head = s->ev_head;
                    cl[i].ev_next = head > FL_EVENTS ? head - FL_EVENTS : 0;   /* replay recent history */
                    placed = 1;
                    break;
                }
            }
            if (fd != -1 && !placed) {
                close(fd);
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (cl[i].fd != -1 && FD_ISSET(cl[i].fd, &rf) && read_client(&cl[i]) == -1) {
                close(cl[i].fd);
                cl[i].fd = -1;
            }
        }

        uint64_t now = fl_now_ns();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (cl[i].fd != -1 && send_events(&cl[i]) == -1) {
                close(cl[i].fd);
                cl[i].fd = -1;
            }
        }
        if (now >= next_state) {
            next_state = now + 50000000ULL;
            int64_t win = __atomic_exchange_n(&s->ctrl_win_late_ns, 0, __ATOMIC_ACQ_REL);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (cl[i].fd != -1 && send_state(cl[i].fd, win) == -1) {
                    close(cl[i].fd);
                    cl[i].fd = -1;
                }
            }
            fl_heartbeat(&hb, me);
        }
    }
}
