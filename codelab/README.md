# Why your QNX TimerTimeout doesn't time out

A hands-on codelab for QNX 8.0. About 30 minutes.

**Follow it step by step at [zuhairhalawa.github.io/faultline/qnx-timertimeout-unblock](https://zuhairhalawa.github.io/faultline/qnx-timertimeout-unblock/).** That page is built from [`qnx-timertimeout-unblock.md`](qnx-timertimeout-unblock.md) (the same claat format as the [QNX codelabs](https://qnx.github.io/codelabs/)) by [`build_site.py`](build_site.py). This README has the same content on one page.

You will build a tiny message-passing server and client, and watch a 5 ms timeout fail to stop a client from waiting 30 ms, then forever. Then you will fix it twice: on the server, so a slow server honours the client's timeout, and on the client, so even a frozen server can't stall a real-time loop.

This came out of [Faultline](../README.md): a pump controller that read its sensor with a 5 ms timeout ran 77 ms late when the sensor froze.

## What you'll learn

- What `_NTO_CHF_UNBLOCK` does, and why every `name_attach()` server has it.
- Why `TimerTimeout()` can't release a REPLY-blocked client by itself.
- How to answer `_PULSE_CODE_UNBLOCK`, and how to make a slow reply a deferred reply instead of a sleep.
- How to keep a periodic loop on time when the server it depends on stops answering.

## What you need

- **QNX SDP 8.0** on Linux or WSL2, with `qcc` working (`source ~/qnx800/qnxsdp-env.sh`).
- **A QNX 8.0 target you can reach with SSH as root**, for example a QEMU image from `mkqnximage --type=qemu`. Every output below comes from QNX 8.0 x86_64 in QEMU with KVM; your timings will differ a little.
- The files in this folder: [`ping.h`](ping.h), [`server.c`](server.c), [`client.c`](client.c), [`Makefile`](Makefile).

---

## Step 1: Build and copy to the target

```sh
source ~/qnx800/qnxsdp-env.sh
make                                    # builds build/x86_64/cl-server and cl-client
ssh root@<target-ip> mkdir -p /data/cl
scp build/x86_64/cl-server build/x86_64/cl-client root@<target-ip>:/data/cl/
```

For a Raspberry Pi, build with `make PLATFORM=aarch64le` and copy from `build/aarch64le/` instead.

Then log in to the target with `ssh root@<target-ip>` and `cd /data/cl`. Each step runs the server in the background (`&`) and the client in the foreground of the same shell, so you can signal both. `$!` holds the pid of the last background process; the steps save it in `SP` (server) and `CP` (client).

The programs:

- `cl-server [-d DELAY_MS] [-u]` registers the name `cl-ping` with `name_attach()` and answers pings. `-d` makes every answer late. `-u` switches on the fix from step 5.
- `cl-client [-t TIMEOUT_MS] [-n COUNT] [-w]` sends a ping every 100 ms. Each `MsgSend()` is bounded by `TimerTimeout()` (5 ms by default) for both the SEND and REPLY states. `-w` is the loop from step 6.

Between steps, stop everything with:

```sh
slay -s SIGCONT cl-server; slay -s SIGKILL cl-server cl-client
```

## Step 2: A healthy server

```sh
./cl-server & SP=$!
./cl-client -n 3
```

```
server: pid 11907101, name cl-ping, delay 0 ms, naive (sleeps, then replies)
client: pid 11919390, timeout 5 ms
ping  1: reply after 0.0 ms
ping  2: reply after 0.0 ms
ping  3: reply after 0.1 ms
```

Nothing surprising yet.

## Step 3: A slow server ignores your timeout

Start the server so that it answers 30 ms late. The client still has a 5 ms timeout.

```sh
./cl-server -d 30 & SP=$!
./cl-client -n 3
```

```
server: pid 11956253, name cl-ping, delay 30 ms, naive (sleeps, then replies)
client: pid 11968542, timeout 5 ms
server: got _PULSE_CODE_UNBLOCK (ignored; already replied)
ping  1: reply after 30.3 ms
server: got _PULSE_CODE_UNBLOCK (ignored; already replied)
ping  2: reply after 31.2 ms
server: got _PULSE_CODE_UNBLOCK (ignored; already replied)
ping  3: reply after 31.3 ms
```

The timeout fired at 5 ms, yet every call took 30 ms, and the server got one `_PULSE_CODE_UNBLOCK` per ping. Here is why:

1. `name_attach()` creates its channel with `_NTO_CHF_UNBLOCK` (along with `_NTO_CHF_DISCONNECT` and `_NTO_CHF_COID_DISCONNECT`). See [`name_attach()`](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html) and [`ChannelCreate()`](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html).
2. Once the server has received a message, the client is **REPLY-blocked**. If the client then wants to stop waiting (its `TimerTimeout()` fires, or it gets a signal), a channel with `_NTO_CHF_UNBLOCK` doesn't let the kernel release it. The kernel sends the server a `_PULSE_CODE_UNBLOCK` pulse instead, and the client stays REPLY-blocked until the server replies or calls `MsgError()`.
3. This server sleeps while it holds the request, so it only sees the pulse after it has already replied. The pulse arrives too late to matter.

The flag exists for a good reason: a server may have work in flight for that client (a DMA transfer, a reply buffer) and needs to clean it up before the client goes away. But it means **the server decides when your timeout takes effect**.

## Step 4: A frozen server holds you forever

Start a healthy server, freeze it with `SIGSTOP`, then start the client in the background and look at it a second later:

```sh
./cl-server & SP=$!
kill -STOP $SP
./cl-client -n 1 & CP=$!
sleep 1
pidin -p cl-client -F "%a %b %N %p %J %B"
```

```
     pid tid name               prio STATE          Blocked
12017694   1 cl-client           10r REPLY          12005405
```

The client is REPLY-blocked on the server's pid, and it stays there. (If you run `pidin` straight away, you may catch it briefly REPLY-blocked on pid 1 while `name_open()` finishes. That's why the step waits a second.)

Notice the client went into `REPLY`, not `SEND`, even though the server was frozen before the client started. The server's thread was waiting in `MsgReceive()` when it was stopped, and the kernel still handed it the message. So the client can't even rely on staying in `SEND`, where its timeout would have worked.

Now try each of these. They are alternatives: after each one, clean up and repeat the commands above.

- **`kill -TERM $CP`** does nothing. `pidin` still shows the client in `REPLY`.
- **`kill -KILL $CP`** does nothing either. On QNX 8.0 in our runs, `pidin` still showed the client REPLY-blocked after `SIGKILL`.
- **`kill -CONT $SP`** lets the server run. It answers the pending request, and only then can the client finish, or act on a signal you sent it.
- **`kill -KILL $SP`** frees the client with an error:

  ```
  ping  1: No such process after 1005.9 ms
  ```

  The request in flight fails with `ESRCH`. Any later `MsgSend()` on the same connection fails at once with `EBADF`, because the connection is gone.

## Step 5: Fix the server

A server that can be slow must never sleep while holding a request. It should:

1. Remember the `rcvid` and go straight back to `MsgReceive()`. This is a **deferred reply**.
2. Arm a timer that sends itself a pulse when the answer is due, and reply then.
3. If `_PULSE_CODE_UNBLOCK` arrives for the held `rcvid` first, release the client with `MsgError()`.

The unblock pulse's value is the `rcvid` of the client that wants to leave. On QNX 8.0 `rcvid_t` is 64-bit, so read it from `sival_long`:

```c
case _PULSE_CODE_UNBLOCK: {
    rcvid_t who = (rcvid_t)msg.pulse.value.sival_long;
    if (who == held) {
        timer_settime(reply_timer, 0, &off, NULL);   /* cancel the pending reply */
        MsgError(held, ETIMEDOUT);                   /* this is what frees the client */
        held = 0;
    }
    break;
}
case PULSE_REPLY_DUE:                                /* our one-shot timer */
    if (held) { MsgReply(held, EOK, &held_reply, sizeof(held_reply)); held = 0; }
    break;
```

and when a request comes in:

```c
held = rcvid;
held_reply = r;
struct itimerspec its = { 0 };
its.it_value.tv_sec = delay_ms / 1000;               /* set fields by name: the member */
its.it_value.tv_nsec = (delay_ms % 1000) * 1000000L; /* order isn't fixed by POSIX */
timer_settime(reply_timer, 0, &its, NULL);
```

The server can't tell whether the client gave up because of a timeout or a signal. This codelab answers `ETIMEDOUT` because its clients use timeouts; a server whose clients are interrupted by signals would usually answer `EINTR`.

The whole thing is in [`server.c`](server.c). Run it with `-u`:

```sh
./cl-server -d 30 -u & SP=$!
./cl-client -n 3
```

```
server: pid 12144669, name cl-ping, delay 30 ms, fixed (deferred replies, honours unblock)
client: pid 12156958, timeout 5 ms
server: client gave up waiting; released it with ETIMEDOUT
ping  1: Connection timed out after 5.5 ms
server: client gave up waiting; released it with ETIMEDOUT
ping  2: Connection timed out after 5.9 ms
server: client gave up waiting; released it with ETIMEDOUT
ping  3: Connection timed out after 6.0 ms
```

The client's 5 ms timeout now works. (It fires a little after 5 ms because timers expire on the system clock tick, 1 ms by default.) A reply that is late but still inside the timeout is delivered normally:

```sh
./cl-server -d 3 -u & SP=$!
./cl-client -n 3
```

```
ping  1: reply after 3.6 ms
ping  2: reply after 4.1 ms
ping  3: reply after 3.5 ms
```

## Step 6: Protect the client from a server that can't answer

The fixed server only helps while it can run. Freeze it and the client is stuck exactly as in step 4: nobody is left to answer the unblock pulse.

So a client with a deadline shouldn't call `MsgSend()` from its time-critical thread, at least not to a server it can't trust to answer. In `cl-client -w`, a 100 Hz loop at `SCHED_FIFO` priority 20 hands each request to a worker thread at priority 15 and never waits for it. It uses whatever answers have arrived, and keeps time either way:

```c
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
/* ... measure how late this tick is ... */
int idle = 0;                                        /* never wait for the server here */
if (atomic_compare_exchange_strong(&busy, &idle, 1)) sem_post(&go);
```

First, with a healthy server:

```sh
./cl-server > /dev/null & SP=$!
./cl-client -w -n 3
```

```
loop: 100 ms window  1, worst tick 1.03 ms late, 9 answers
loop: 100 ms window  2, worst tick 0.94 ms late, 10 answers
loop: 100 ms window  3, worst tick 0.93 ms late, 10 answers
```

About 1 ms of lateness is the clock tick again: an absolute sleep wakes on the next tick. Now freeze the server 150 ms into the run:

```sh
./cl-server > /dev/null & SP=$!
./cl-client -w -n 5 & CP=$!
sleep 0.15; kill -STOP $SP
```

```
loop: 100 ms window  1, worst tick 1.13 ms late, 9 answers
loop: 100 ms window  2, worst tick 0.98 ms late, 6 answers
loop: 100 ms window  3, worst tick 1.01 ms late, 0 answers
loop: 100 ms window  4, worst tick 1.05 ms late, 0 answers
loop: 100 ms window  5, worst tick 1.10 ms late, 0 answers
```

The answers stop, but the loop stays on time. In a real controller, "0 answers" is what your fault handling reacts to: hold the last value, then go to a safe state.

One more thing `pidin` shows you. When the loop finishes, the client can't exit:

```sh
pidin -p cl-client -F "%a %b %N %p %J %B"
```

```
     pid tid name               prio STATE          Blocked
12304414   2 cl-client           15f REPLY          12292125
```

Its worker thread (tid 2, priority 15 FIFO) is still REPLY-blocked on the frozen server, and as in step 4, even `SIGKILL` doesn't free it. What does free it is dealing with the server: resume it, or kill it. That's the job of a supervisor that watches heartbeats and kills a server that stops sending them. [Faultline](../README.md) does exactly that with its `fl-supervisor`.

## Recap

- Every `name_attach()` channel has `_NTO_CHF_UNBLOCK`. Once the server has your message, **your `TimerTimeout()` only works if the server answers the unblock pulse.**
- **In servers:** never sleep or block while holding a request. Use deferred replies, and answer `_PULSE_CODE_UNBLOCK` with `MsgError()`.
- **In clients with deadlines:** do the `MsgSend()` from a lower-priority worker thread, and decide from fresh data, not from whether the call returned.
- **In the system:** a frozen server can't release anyone, not even a client being killed. Detect it (heartbeats) and kill it. That fails the request in flight with `ESRCH`, and the client can reconnect to a restarted server.

## Clean up

```sh
slay -s SIGCONT cl-server; slay -s SIGKILL cl-server cl-client
rm -rf /data/cl
```
