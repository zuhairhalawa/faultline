# Why your QNX TimerTimeout doesn't time out

A hands-on codelab for QNX 8.0. About 30 minutes.

You will build a tiny message-passing server and client, and watch a 5 ms timeout fail to stop a client from waiting 30 ms, then forever. Then you will fix it twice: on the server, so a slow server honours the client's timeout, and on the client, so even a frozen server can't stall a real-time loop.

This came out of [Faultline](../README.md): a pump controller that read its sensor with a 5 ms timeout ran 77 ms late when the sensor froze.

## What you'll learn

- What `_NTO_CHF_UNBLOCK` does, and why every `name_attach()` server has it.
- Why `TimerTimeout()` can't release a REPLY-blocked client by itself.
- How to answer `_PULSE_CODE_UNBLOCK`, and how to make a slow reply a deferred reply instead of a sleep.
- How to keep a periodic loop on time when the server it depends on stops answering.

## What you need

- **QNX SDP 8.0** on Linux or WSL2, with `qcc` working (`source ~/qnx800/qnxsdp-env.sh`).
- **A QNX 8.0 target you can reach with SSH**, for example a QEMU image from `mkqnximage --type=qemu`. Every output below comes from QNX 8.0 x86_64 in QEMU with KVM; your timings will differ a little.
- The files in this folder: [`ping.h`](ping.h), [`server.c`](server.c), [`client.c`](client.c), [`Makefile`](Makefile).

---

## Step 1: Build and copy to the target

```sh
source ~/qnx800/qnxsdp-env.sh
make                                    # x86_64; PLATFORM=aarch64le for a Raspberry Pi
ssh root@<target-ip> mkdir -p /data/cl
scp build/x86_64/server build/x86_64/client root@<target-ip>:/data/cl/
```

Open **two SSH sessions** to the target and `cd /data/cl` in both. Run the server in the first and the client in the second.

The programs:

- `server [-d DELAY_MS] [-u]` registers the name `cl-ping` with `name_attach()` and answers pings. `-d` makes every answer late. `-u` switches on the fix from step 5.
- `client [-t TIMEOUT_MS] [-n COUNT] [-w]` sends a ping every 100 ms. Each `MsgSend()` is bounded by `TimerTimeout()` (5 ms by default) for both the SEND and REPLY states. `-w` is the loop from step 6.

## Step 2: A healthy server

```sh
./server                 # session 1
./client -n 3            # session 2
```

```
client: pid 9592862, timeout 5 ms
ping  1: reply after 0.0 ms
ping  2: reply after 0.0 ms
ping  3: reply after 0.0 ms
```

Nothing surprising yet.

## Step 3: A slow server ignores your timeout

Stop the server with Ctrl-C and start it again, answering 30 ms late. The client still has a 5 ms timeout.

```sh
./server -d 30           # session 1
./client -n 3            # session 2
```

```
ping  1: reply after 30.7 ms
ping  2: reply after 31.2 ms
ping  3: reply after 30.6 ms
```

and in the server's session:

```
server: got _PULSE_CODE_UNBLOCK (ignored; already replied)
```

The timeout fired at 5 ms, yet every call took 30 ms. Here is why:

1. `name_attach()` creates its channel with `_NTO_CHF_UNBLOCK` (along with `_NTO_CHF_DISCONNECT` and `_NTO_CHF_COID_DISCONNECT`). See [`name_attach()`](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html) and [`ChannelCreate()`](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html).
2. Once the server has received a message, the client is **REPLY-blocked**. If the client then wants to stop waiting (its `TimerTimeout()` fires, or it gets a signal), a channel with `_NTO_CHF_UNBLOCK` doesn't let the kernel release it. The kernel sends the server a `_PULSE_CODE_UNBLOCK` pulse instead, and the client stays REPLY-blocked until the server replies or calls `MsgError()`.
3. This server sleeps while it holds the request, so it only sees the pulse after it has already replied. The pulse arrives too late to matter.

The flag exists for a good reason: a server may have work in flight for that client (a DMA transfer, a reply buffer) and needs to clean it up before the client goes away. But it means **the server decides when your timeout takes effect**.

## Step 4: A frozen server holds you forever

Restart the healthy server, freeze it with `SIGSTOP`, then run the client:

```sh
./server &               # session 1; note the pid it prints
kill -STOP <server-pid>
./client -n 3 &          # session 2
pidin -p client -F "%a %b %N %p %J %B"
```

```
     pid tid name               prio STATE          Blocked
 9691166   1 client              10r REPLY          9678877
```

The client is REPLY-blocked on the server's pid, and stays there. Some things worth trying while it's stuck:

- **`kill -TERM <client-pid>`** does nothing. The client is still `REPLY`.
- **`kill -KILL <client-pid>`** does nothing either. On QNX 8.0 in our runs, `pidin` still showed the client REPLY-blocked after `SIGKILL`.
- **`kill -CONT <server-pid>`** lets the server run. It answers the pending request, and only then can the client act on its signals or exit.
- **`kill -KILL <server-pid>`** frees the client with an error:

  ```
  ping  1: No such process after 1006.1 ms
  ```

Notice the client went into `REPLY`, not `SEND`, even though the server was frozen before the client started. The server's thread was waiting in `MsgReceive()` when it was stopped, and the kernel still handed it the message. So the client can't even rely on staying in `SEND`, where its timeout would have worked.

## Step 5: Fix the server

A server that can be slow must never sleep while holding a request. It should:

1. Remember the `rcvid` and go straight back to `MsgReceive()`. This is a **deferred reply**.
2. Arm a timer that sends itself a pulse when the answer is due, and reply then.
3. If `_PULSE_CODE_UNBLOCK` arrives for the held `rcvid` first, release the client with `MsgError(rcvid, ETIMEDOUT)`.

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

The whole thing is in [`server.c`](server.c). Run it with `-u`:

```sh
./server -d 30 -u        # session 1
./client -n 3            # session 2
```

```
ping  1: Connection timed out after 5.5 ms
ping  2: Connection timed out after 5.9 ms
ping  3: Connection timed out after 6.1 ms
```

```
server: client gave up waiting; released it with ETIMEDOUT
```

The client's 5 ms timeout now works. A reply that is late but still inside the timeout is delivered normally:

```sh
./server -d 3 -u
./client -n 3
```

```
ping  1: reply after 3.6 ms
ping  2: reply after 4.3 ms
ping  3: reply after 4.0 ms
```

## Step 6: Protect the client from a server that can't answer

The fixed server only helps while it can run. Freeze it again and the client is stuck exactly as in step 4: nobody is left to answer the unblock pulse.

So a client with a deadline must never call `MsgSend()` from its time-critical thread. In `client -w`, a 100 Hz loop hands each request to a worker thread and never waits for it. It uses whatever answers have arrived, and keeps time either way:

```c
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
/* ... measure how late this tick is ... */
if (!busy) { busy = 1; sem_post(&go); }   /* the worker does the MsgSend */
```

Run it, then freeze the server while it runs:

```sh
./server &                         # session 1
./client -w -n 5 &                 # session 2
sleep 0.15; kill -STOP <server-pid>
```

```
loop: 100 ms window  1, worst tick 0.72 ms late, 9 answers
loop: 100 ms window  2, worst tick 0.81 ms late, 5 answers
loop: 100 ms window  3, worst tick 0.65 ms late, 0 answers
loop: 100 ms window  4, worst tick 0.87 ms late, 0 answers
loop: 100 ms window  5, worst tick 0.62 ms late, 0 answers
```

The answers stop, but the loop stays on time. In a real controller, "0 answers" is what your fault handling reacts to: hold the last value, then go to a safe state.

One more thing `pidin` shows you. When the loop finishes, the client can't exit:

```
     pid tid name               prio STATE          Blocked
10338334   2 client              10r REPLY          10326045
```

Its worker thread (tid 2) is still REPLY-blocked on the frozen server, and as in step 4, even `SIGKILL` doesn't free it. What does free it is dealing with the server: resume it, or kill it. That's the job of a supervisor that watches heartbeats and kills a server that stops sending them. [Faultline](../README.md) does exactly that with its `fl-supervisor`.

## Recap

- Every `name_attach()` channel has `_NTO_CHF_UNBLOCK`. Once the server has your message, **your `TimerTimeout()` only works if the server answers the unblock pulse.**
- **In servers:** never sleep or block while holding a request. Use deferred replies, and answer `_PULSE_CODE_UNBLOCK` with `MsgError()`.
- **In clients with deadlines:** do the `MsgSend()` from a worker thread, and decide from fresh data, not from whether the call returned.
- **In the system:** a frozen server can't release anyone, not even a client being killed. Detect it (heartbeats) and kill it; that frees its clients with `ESRCH`.

## Clean up

```sh
slay -s SIGCONT server; slay -s SIGKILL server client
rm -rf /data/cl
```
