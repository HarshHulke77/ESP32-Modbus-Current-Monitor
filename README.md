# ESP32 Modbus RTU Industrial Current Monitor

An ESP32 acting as a Modbus RTU **slave**, reporting live current draw (via an ACS712-05B hall-effect sensor) to a Modbus **master** over RS-485. Validated against QModMaster on a PC via an FT232 USB-RS485 adapter — 12/12 read passes.

## Architecture

Two FreeRTOS tasks, pinned to core 1, sharing one value through a mutex:

| Task | Priority | Job |
|---|---|---|
| `TaskReadCurrent` | 1 | Samples the ACS712 (50 readings/cycle), converts to mA, writes to shared state under mutex, every 500 ms |
| `TaskModbusService` | 2 | Reads shared state under mutex, updates the Modbus holding register, services the Modbus RTU protocol (`mb.task()`), every 10 ms |

No queue between the tasks — just a mutex-protected shared variable (`g_currentMilliamps`), since only the latest reading matters for this application (no history requirement, unlike Project 1's MQTT telemetry).

## Wiring

| ESP32 pin | Connects to | Notes |
|---|---|---|
| GPIO34 (ADC1_CH6) | ACS712 OUT, via 10 kΩ/22 kΩ divider | Input-only pin, safe for analog read |
| GPIO26 | MAX485 RO | Modbus RX — moved off GPIO16 after UART0 conflict (see Known Issues) |
| GPIO27 | MAX485 DI | Modbus TX |
| GPIO4 | MAX485 DE + RE (tied together) | HIGH = transmit, LOW = receive |

### Why the divider

The ACS712-05B is a 5 V ratiometric sensor — 2.5 V at 0 A, ±185 mV/A. Fed directly into the ESP32's 3.3 V ADC, the zero-current point already sits outside the ADC's reliable linear range, and anything above +4.3 A exceeds the ADC's absolute maximum input voltage. A 10 kΩ/22 kΩ divider maps the full ±5 A range into roughly 1.08–2.36 V, safely inside the ADC's linear window. Caught on paper before wiring — see Bug Journal entry 002.

## Modbus register map

| Register | Address | Type | Contents |
|---|---|---|---|
| Holding register | 0 | `uint16_t` (stores a cast `int16_t`) | Current, in mA |

**Register 1 (status flag) was designed but is not implemented.** The intended contract: on boot, if fresh calibration fails a sanity check against the last-known-good value, the device should fall back to last-known-good, keep running (not refuse to boot — the current reading is watched by a human via QModMaster, not driving an automated actuator), and flag the degraded state in a second holding register (0 = good calibration, 1 = failed/degraded). The fallback logic itself is implemented (see Calibration below); only the register that would surface it to the Modbus master is missing. Left as an open item rather than silently dropped.

## Calibration

Three-tier fallback, run once at boot after Modbus is already initialized (calibration must run with the RS-485 direction-switching active, not before — a boot-order bug found during bring-up):

1. **Fresh average** — 500 samples of the ACS712 output with zero current applied.
2. **Sanity check** — if the fresh average is within ±0.15 V of the last-known-good value stored in NVS (key `"zeroV"`), accept it. If drift exceeds 0.015 V, write the new value back to NVS.
3. **Reject path** — if the fresh average is *not* within the sanity window, discard it and fall back to the NVS last-known-good value.
4. **Hardcoded fallback** — if NVS has never been written, the last-known-good default is `1.7952 V`.

## Known issues / limitations

- **`SENSITIVITY_V_PER_A = 0.1272 V/A` is unvalidated.** It's derived from the ACS712 datasheet's 0.185 V/A spec, scaled by the divider ratio (22/32) — not from a measured two-point calibration against a known load. Treat current readings as directionally correct, not calibrated-accurate, until validated against a known load.
- **Register 1 status flag is unbuilt** (see register map above).
- **No history/logging** — only the latest reading is ever available; a master that polls infrequently will miss transients.
- Debug `Serial.printf` calls are still present in both tasks; harmless but not production-clean.

## Bugs found during bring-up

Full write-ups with falsification steps are in the personal bug journal. Summary:

| # | Bug | Root cause | Fix |
|---|---|---|---|
| 003a | Modbus lines dead on GPIO16/17 | GPIO16/17 are UART0, shared with USB-serial | Moved to GPIO26/27 |
| 003b | Current readings stuck near 0 | ACS712 IP+/IP− leads reversed | Swapped IP+/IP− direction (confirmed wiring intact via multimeter first) |
| 003c | Never got a negative reading | Current stored as `uint16_t` (unsigned) | Changed to signed int |
| 003d | Readings corrupted after adding a ground wire | Invalid FT232-to-Arduino-Uno ground tie introduced a ground fault | Removed the FT232 ground connection |
| — | ADC range mismatch | ACS712's 5 V output incompatible with ESP32's 3.3 V ADC | Caught before wiring, from the datasheet — divider added |

## Hardware validation

12/12 QModMaster read requests confirmed register 0 tracking live current correctly, via FT232 USB-RS485 adapter.
