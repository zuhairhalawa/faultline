# Why your QNX TimerTimeout doesn't time out

A hands-on codelab for QNX 8.0, about 50 minutes.

**[Start the codelab](https://zuhairhalawa.github.io/faultline/qnx-timertimeout-unblock/)**

A client sends a request with a 5 ms timeout. The server takes 30 ms. On QNX the client often waits the full 30 ms, and if the server freezes, it can wait forever, even through `SIGKILL`. In this codelab you watch that happen on a real QNX 8.0 target, learn why (`_NTO_CHF_UNBLOCK`, which every `name_attach()` channel has), and fix it twice:

- **In the server**, which you write yourself: keep slow requests as deferred replies and answer `_PULSE_CODE_UNBLOCK` with `MsgError()`.
- **In the client:** a `SCHED_FIFO` control loop that hands `MsgSend()` to a worker thread, so a frozen server costs it data, not deadlines.

It has predict-then-run checks, a hands-on exercise with expected output at each stage, sequence diagrams, common mistakes and a short knowledge check. Every output in it comes from runs on QNX 8.0 x86_64 in QEMU.

## Files

| File | What it is |
| --- | --- |
| [`qnx-timertimeout-unblock.md`](qnx-timertimeout-unblock.md) | The codelab, in the same claat format as the [QNX codelabs](https://qnx.github.io/codelabs/) |
| [`server.c`](server.c) | `cl-server`: the ping server; `-d` makes it slow, `-u` turns on the fix |
| [`server-todo.c`](server-todo.c) | `cl-server-todo`: the same server with the fix left as two TODOs, for the exercise |
| [`client.c`](client.c) | `cl-client`: pings with a `TimerTimeout()`; `-w` runs the worker-thread control loop |
| [`ping.h`](ping.h), [`Makefile`](Makefile) | The message and the `qcc` build |
| [`img/`](img/) | The sequence diagrams |
| [`build_site.py`](build_site.py) | Renders the codelab page: `python3 codelab/build_site.py <output-dir>` |

## Build

```sh
source ~/qnx800/qnxsdp-env.sh
make                        # build/x86_64/cl-server, cl-client, cl-server-todo
make PLATFORM=aarch64le     # for a Raspberry Pi
```

It came out of [Faultline](../README.md), where a pump controller with a 5 ms sensor timeout ran 77 ms late when the sensor froze.
