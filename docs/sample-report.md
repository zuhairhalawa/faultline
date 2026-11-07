# Faultline report: Infusion pump, single faults

Run 2026-11-08 03:40 against target 192.168.122.80.
System: Simulated infusion pump at 120 mL/h: sensor, motor driver and 100 Hz controller as separate QNX processes.
Environment: QNX SDP 8.0, x86_64, QEMU with KVM, 4 virtual CPUs.

**Overall: PASS** (9 of 9 steps as required)

| Step | Fault | Detected | Back in service | Worst tick late | Pump | Dose error | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Motor driver crashes | `crash motor-drv` | crash in 0.53 ms | 2.26 ms | 0.0 ms | kept running | -0.34% | **PASS** |
| Flow sensor crashes | `crash flow-sensor` | crash in 0.53 ms | 1.96 ms | 0.0 ms | kept running | -0.21% | **PASS** |
| Controller crashes | `crash pump-ctrl` | crash in 0.57 ms | 2.81 ms | 0.0 ms | kept running | 1.22% | **PASS** |
| Flow sensor freezes | `hang flow-sensor` | hang in 93.58 ms | 96.03 ms | 0.0 ms | held at 56.61 ms | -0.4% | **PASS** |
| Motor driver freezes | `hang motor-drv` | hang in 102.19 ms | 110.22 ms | 0.0 ms | kept running | -0.21% | **PASS** |
| Sensor replies 30 ms late | `slow 30 800` | – | – | 0.0 ms | kept running | 0.15% | **PASS** |
| CPU flood below the controller's priority | `flood 30 200` | – | – | 0.0 ms | kept running | -0.15% | **PASS** |
| Sensor stuck on one value | `stuck 600` | – | – | 0.0 ms | alarm at 299.51 ms | -28.58% | **PASS** |
| CPU flood above the controller's priority | `flood 41 80` | – | – | 60.85 ms | held at 80.38 ms | 0.44% | **CAUGHT** |

## Details

### Motor driver crashes: PASS

Fault `crash motor-drv`. Limits: recover_ms 20, max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject crash motor-drv
+0.53 ms  motor-drv died (signal 11), seen 0.52 ms after fault
+1.25 ms  motor-drv restarted after crash
+2.26 ms  motor-drv back 2.26 ms after fault
```

### Flow sensor crashes: PASS

Fault `crash flow-sensor`. Limits: recover_ms 20, max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject crash flow-sensor
+0.53 ms  flow-sensor died (signal 11), seen 0.53 ms after fault
+1.22 ms  flow-sensor restarted after crash
+1.96 ms  flow-sensor back 1.96 ms after fault
```

### Controller crashes: PASS

Fault `crash pump-ctrl`. Limits: recover_ms 20, alarm False, dose_err_pct 2.

```
+0.00 ms  inject crash pump-ctrl
+0.57 ms  pump-ctrl died (signal 11), seen 0.57 ms after fault
+1.05 ms  pump-ctrl restarted after crash
+2.81 ms  pump-ctrl back 2.81 ms after fault
```

### Flow sensor freezes: PASS

Fault `hang flow-sensor`. Limits: recover_ms 150, max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject hang flow-sensor
+56.61 ms  sensor lost, holding last speed
+93.58 ms  flow-sensor missed its 100 ms heartbeat
+94.98 ms  flow-sensor restarted after hang
+96.03 ms  flow-sensor back 96.03 ms after fault
+106.30 ms  sensor back, pump running
```

### Motor driver freezes: PASS

Fault `hang motor-drv`. Limits: recover_ms 150, max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject hang motor-drv
+102.19 ms  motor-drv missed its 100 ms heartbeat
+103.36 ms  motor-drv restarted after hang
+110.23 ms  motor-drv back 110.22 ms after fault
```

### Sensor replies 30 ms late: PASS

Fault `slow 30 800`. Limits: max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject slow sensor replies +30 ms for 800 ms
+808.08 ms  slow sensor replies ended
```

### CPU flood below the controller's priority: PASS

Fault `flood 30 200`. Limits: max_late_ms 5, alarm False.

```
+0.00 ms  inject flood prio 30 for 200 ms on 4 CPUs
+218.89 ms  CPU flood at priority 30 ended
```

### Sensor stuck on one value: PASS

Fault `stuck 600`. Limits: alarm True, alarm_within_ms 400.

```
+0.00 ms  inject stuck sensor for 600 ms
+109.66 ms  sensor lost, holding last speed
+299.51 ms  no sensor for 200 ms: motor stopped, alarm
+619.52 ms  sensor back, pump running
+635.83 ms  stuck sensor ended
```

### CPU flood above the controller's priority: CAUGHT

Fault `flood 41 80`. Limits: max_late_ms 5.
This step is meant to break a limit; it passes when Faultline catches the breach.

- control tick 60.85 ms late, limit 5 ms

```
+0.00 ms  inject flood prio 41 for 80 ms on 4 CPUs
+80.38 ms  sensor lost, holding last speed
+80.38 ms  1 control tick late, worst 60.9 ms
+90.26 ms  sensor back, pump running
+91.28 ms  CPU flood at priority 41 ended
```

## Notes

Times come from the QNX target's CLOCK_MONOTONIC. Under QEMU they are approximate:
the emulator doesn't run in real time, so the same build on real hardware gives tighter numbers.
The pump is a simulation used to show recovery behaviour, not a medical device.
