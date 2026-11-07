# Faultline design

Faultline breaks a running QNX system on purpose and measures whether it recovers in time. It ships with a reference system, a simulated infusion pump, so the whole loop can be shown end to end.

Target: QNX SDP 8.0, x86_64, running in QEMU. Host: Ubuntu 24.04 in WSL2 on Windows 11.

## What it has to prove

For each fault, the report answers four questions:

1. How long until the fault was detected?
2. How long until the faulty process was running again?
3. Did the safety-critical loop keep its deadline the whole time?
4. Did the patient-facing outcome stay within limits? For the pump, that's the delivered dose compared with the prescribed dose.

Each answer is checked against limits written in the campaign file, and the report says pass or fail.

## Parts

### On the QNX target (C, built with qcc)

| Process | Priority (SCHED_FIFO) | Job | QNX features used |
| --- | --- | --- | --- |
| `fl-plant` | 60 | The physical world: pump motor inertia and fluid flow, updated at 1 kHz in shared memory. Faultline never faults it, because hardware doesn't crash when its driver does. | `timer_create` with a pulse, `shm_open` |
| `fl-supervisor` | 55 | Starts every process listed in `system.conf`, notices deaths and missed heartbeats, restarts processes, and timestamps every step. | `procmgr_event_notify(PROCMGR_EVENT_PROCESS_DEATH)`, `MsgRegisterEvent`, heartbeat pulses, `posix_spawn`, `ClockCycles` |
| `motor-drv` | 45 | Driver for the pump motor. Exposed as `/dev/fl/motor`, which you can read and write like a file. | Resource manager (`resmgr_attach`, `iofunc_*`) |
| `flow-sensor` | 42 | Reads flow from the plant, adds noise, and answers requests from the controller. | `name_attach`, `MsgReceive`/`MsgReply` |
| `pump-ctrl` | 40 | 100 Hz control loop. Asks the sensor for flow with a 5 ms timeout and sets motor speed. If the sensor is lost, it holds the last speed for up to 200 ms, then stops the pump safely and raises an alarm. | `name_open`, `MsgSend`, `TimerTimeout` |
| `fl-inject` | 50 | Injects faults on command. Exposed as `/dev/faultline/inject`, so `echo "crash motor-drv" > /dev/faultline/inject` works from the shell. | Resource manager, `kill`, priority control |
| `fl-bridge` | 20 | TCP server on port 7700. Streams telemetry as JSON lines and accepts commands from the dashboard. | sockets |

`libfl` holds the shared pieces: heartbeat helper, time conversion, and a telemetry ring in shared memory.

### Faults

| Fault | How it is injected | What should happen |
| --- | --- | --- |
| Crash | `SIGSEGV` to the process | The supervisor sees the death event and restarts it |
| Hang | `SIGSTOP` | The heartbeat deadline is missed, the supervisor kills and restarts it |
| CPU flood | A busy thread at a chosen priority for a chosen time | Higher-priority work keeps its deadline. If the flood sits above the controller, the miss is measured and reported |
| Slow replies | A flag in shared memory makes a server delay its replies | The controller's timeout fires and it falls back as designed |
| Stuck sensor | The sensor repeats one value | The controller's plausibility check flags it |

### On the host (Node.js, no outside packages)

- **Dashboard:** a local web page.
  - Process tiles show state, PID and restart count.
  - A live chart shows prescribed and delivered flow.
  - Fault buttons, and a timeline with detection and recovery times for each fault.
- **Campaign runner:** reads `campaign.json`, a list of faults with timing and limits. It runs them in order and writes a report in Markdown and HTML.
  - The runner also works from the command line, so a team could run it in CI.

## Honest limits

- QEMU doesn't run in real time, so the timings it measures are approximate. The report records the environment and says so. On real hardware, the same code would give tighter numbers.
- The pump is a simulation for showing recovery behaviour. It isn't a medical device and makes no clinical claims.

## Scope by deadline (Dec 15, 2026)

1. Pump runs on QNX in QEMU, with the supervisor restarting crashed processes.
2. Hang detection, the fault injector, and the bridge.
3. Dashboard and campaign report.
4. CPU flood, slow replies and stuck sensor.
5. Video, README and submission.

If time runs short, step 4 shrinks before anything else does.
