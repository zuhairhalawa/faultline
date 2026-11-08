# Faultline report: Infusion pump, single faults

Run 2026-11-08 12:55 against target 192.168.122.80.
System: Simulated infusion pump at 120 mL/h: sensor, motor driver and 100 Hz controller as separate QNX processes.
Environment: QNX SDP 8.0, x86_64, QEMU with KVM, 4 virtual CPUs.

**Overall: PASS** (9 of 9 steps as required)

| Step | Fault | Detected | Back in service | Worst tick late | Pump | Dose error | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Motor driver crashes | `crash motor-drv` | crash in 0.68 ms | 2.61 ms | 2.38 ms | kept running | -0.15% | **PASS** |
| Flow sensor crashes | `crash flow-sensor` | crash in 0.49 ms | 3.09 ms | 2.48 ms | kept running | 0.0% | **PASS** |
| Controller crashes | `crash pump-ctrl` | crash in 1.0 ms | 3.69 ms | n/a (controller restarted) | kept running | 1.01% | **PASS** |
| Flow sensor freezes | `hang flow-sensor` | hang in 104.74 ms | 106.89 ms | 0.46 ms | held at 52.85 ms | 0.07% | **PASS** |
| Motor driver freezes | `hang motor-drv` | hang in 97.65 ms | 99.92 ms | 0.59 ms | kept running | 0.43% | **PASS** |
| Sensor replies 30 ms late for 150 ms | `slow 30 150` | – | – | 0.72 ms | held at 68.35 ms | -0.74% | **PASS** |
| CPU flood below the controller's priority | `flood 30 200` | – | – | 0.46 ms | kept running | 0.36% | **PASS** |
| Sensor stuck on one value | `stuck 600` | – | – | 0.55 ms | alarm at 300.03 ms | -26.97% | **PASS** |
| CPU flood above the controller's priority | `flood 41 80` | – | – | 70.07 ms | held at 80.56 ms | 1.25% | **CAUGHT** |

## Details

### Motor driver crashes: PASS

Fault `crash motor-drv`. Limits: recover_ms 20, max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject crash motor-drv
+0.68 ms  motor-drv died (signal 11), seen 0.66 ms after fault
+1.59 ms  motor-drv restarted after crash
+2.61 ms  motor-drv back 2.61 ms after fault
```

### Flow sensor crashes: PASS

Fault `crash flow-sensor`. Limits: recover_ms 20, max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject crash flow-sensor
+0.49 ms  flow-sensor died (signal 11), seen 0.48 ms after fault
+1.62 ms  flow-sensor restarted after crash
+3.09 ms  flow-sensor back 3.09 ms after fault
```

### Controller crashes: PASS

Fault `crash pump-ctrl`. Limits: recover_ms 20, alarm False, dose_err_pct 2.

```
+0.00 ms  inject crash pump-ctrl
+1.00 ms  pump-ctrl died (signal 11), seen 0.99 ms after fault
+2.25 ms  pump-ctrl restarted after crash
+3.68 ms  pump-ctrl back 3.69 ms after fault
```

### Flow sensor freezes: PASS

Fault `hang flow-sensor`. Limits: recover_ms 150, max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject hang flow-sensor
+52.85 ms  sensor lost, holding last speed
+104.74 ms  flow-sensor missed its 100 ms heartbeat
+105.96 ms  flow-sensor restarted after hang
+106.89 ms  flow-sensor back 106.89 ms after fault
+122.58 ms  sensor back, pump running
```

### Motor driver freezes: PASS

Fault `hang motor-drv`. Limits: recover_ms 150, max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject hang motor-drv
+97.65 ms  motor-drv missed its 100 ms heartbeat
+98.99 ms  motor-drv restarted after hang
+99.92 ms  motor-drv back 99.92 ms after fault
```

### Sensor replies 30 ms late for 150 ms: PASS

Fault `slow 30 150`. Limits: max_late_ms 5, alarm False, dose_err_pct 2.

```
+0.00 ms  inject slow sensor replies +30 ms for 150 ms
+68.35 ms  sensor lost, holding last speed
+168.64 ms  sensor back, pump running
+174.26 ms  slow sensor replies ended
```

### CPU flood below the controller's priority: PASS

Fault `flood 30 200`. Limits: max_late_ms 5, alarm False.

```
+0.00 ms  inject flood prio 30 for 200 ms on 4 CPUs
+200.60 ms  CPU flood at priority 30 ended
```

### Sensor stuck on one value: PASS

Fault `stuck 600`. Limits: alarm True, alarm_within_ms 400.

```
+0.00 ms  inject stuck sensor for 600 ms
+90.26 ms  sensor lost, holding last speed
+300.03 ms  no sensor for 200 ms: motor stopped, alarm
+620.00 ms  sensor back, pump running
+645.86 ms  stuck sensor ended
```

### CPU flood above the controller's priority: CAUGHT

Fault `flood 41 80`. Limits: max_late_ms 5.
This step is meant to break a limit; it passes when Faultline catches the breach.

- control tick 70.07 ms late, limit 5 ms

```
+0.00 ms  inject flood prio 41 for 80 ms on 4 CPUs
+80.56 ms  sensor lost, holding last speed
+80.57 ms  1 control tick late, worst 70.1 ms
+80.85 ms  sensor back, pump running
+80.86 ms  CPU flood at priority 41 ended
```

## Notes

Worst tick late covers every 100 Hz control tick in the step's window; the deadline is 5 ms.
Times come from the QNX target's CLOCK_MONOTONIC. Under QEMU they are approximate:
the emulator doesn't run in real time, so the same build on real hardware gives tighter numbers.
The pump is a simulation used to show recovery behaviour, not a medical device.
