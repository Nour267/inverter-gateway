# Inverter Gateway

A small embedded-style **IoT gateway in C on Linux**. A simulated solar inverter sends
measurements over **CAN bus**. The gateway decodes and validates them, packs them into a custom
framed protocol with a **CRC16**, and streams them to a **cloud server over TCP**. It reconnects
automatically when the cloud goes away.

```
┌──────────────────┐   CAN (SocketCAN, vcan0)   ┌────────────────────────┐   TCP, custom frames   ┌──────────────────┐
│  inverter_sim    │ ─────────────────────────► │        gateway         │ ─────────────────────► │ cloud_server.py  │
│  (fake inverter) │   0x100 STATUS  every 1 s  │ decode → validate →    │   AA 55 | hdr |        │ parse → CRC →    │
│  sun curve,      │   0x101 POWER   every 1 s  │ combine → pack + CRC → │   payload | CRC16      │ print, detect    │
│  faults, bad     │                            │ send, reconnect (2 s)  │                        │ lost frames      │
│  frames          │                            │                        │                        │                  │
└──────────────────┘                            └────────────────────────┘                        └──────────────────┘
       C                                                   C                                            Python
```

## Features

- **CAN decoding** of two inverter messages: little-endian, scaled integers (0.1 V, 0.01 A,
  0.1 °C), signed temperature, length and range validation
- **Hardware abstraction layer**: the gateway only talks to `bus.h`. SocketCAN today; a real
  CAN driver would be one new file
- **Uplink protocol**: start-of-frame, version, type, sequence number, length, payload,
  CRC-16/CCITT. Big-endian. Byte-by-byte **parser state machine** that handles split frames,
  garbage and corrupted data, and resynchronizes
- **Gateway**: single-threaded `poll()` event loop, stale-data detection (no data older than
  3 s is sent), counters, logging, clean shutdown on Ctrl+C
- **Robust TCP client**: reconnect every 2 s, `SIGPIPE` ignored, partial sends handled
- **Cloud server** in Python with its own parser, CRC check and lost-frame detection (`seq` gaps)
- **33 unit tests** (Unity) + a one-command **integration test**

## Build and run

Requirements: Linux (or WSL2), `gcc`, `make`, `python3`, `can-utils`.

```bash
sudo ./scripts/setup_vcan.sh     # create the virtual CAN bus vcan0 (once per boot)
make                             # build gateway and inverter_sim
make test                        # unit tests (no CAN or network needed)
make demo                        # integration test: all 3 programs, PASS/FAIL
```

Run it by hand (3 terminals):

```bash
python3 cloud/cloud_server.py                # 1. the cloud
./build/gateway                              # 2. the gateway (--bus vcan0 --server 127.0.0.1:5000)
./build/inverter_sim                         # 3. the inverter (--fault-at N, --bad-frames)
candump vcan0                                # optional: watch the raw CAN frames
```

Example server output:

```
seq=18    20:58:53  STARTING   354.8 V DC   0.81 A    278 W  230.1 V AC   26.3 C
seq=19    20:58:54  FAULT        0.0 V DC   0.00 A      0 W  229.7 V AC   25.2 C  FAULT code 17
SUMMARY frames=22 crc_errors=0 other_errors=0 lost=0
```

## Design decisions

| Decision | Why |
|---|---|
| Scaled integers, not floats | Many microcontrollers have no FPU; integers are exact and have a fixed size on the wire |
| Pack byte by byte with shifts, not `memcpy` of a struct | Struct padding and CPU byte order differ between machines; shifts give the same bytes everywhere |
| Length-prefixed frames + SOF | TCP is a byte stream: one `recv()` may hold half a frame or two frames |
| CRC16 on top of TCP | Catches our own bugs, allows resync, and the protocol could later run over UART or cellular |
| Parser fed one byte at a time | Works with any chunk size; no dynamic memory |
| `poll()` loop, not threads | No locks or race conditions; easy to reason about. Common in embedded systems |
| Validate before forwarding | Out-of-range or malformed CAN data is dropped and counted, never sent to the cloud |
| No `malloc` | Fixed-size buffers: memory use is known in advance |
| `src/common/` has no I/O | All logic is unit-testable on any PC, without hardware |

**Known limitations (P1):** `connect()` is blocking (instant on localhost; a product would use a
non-blocking connect). Frames built while disconnected are dropped (the server reports the `seq`
gap). Both are addressed in the roadmap.

## Project layout

```
include/            headers: the interfaces (bus.h, can_messages.h, protocol.h, crc16.h, uplink.h)
src/common/         pure logic, no I/O, unit-tested: CAN decode/encode, CRC16, protocol pack/parse
src/bus/            the HAL implementation: SocketCAN
src/gateway/        the gateway: main loop (main.c) and TCP uplink (uplink.c)
src/sim/            the inverter simulator
cloud/              the Python cloud server
tests/              Unity unit tests (tests/unity/ is the vendored framework)
scripts/            setup_vcan.sh, run_demo.sh
DESIGN.md           full design: message map, protocol, internals, milestones
```

## Roadmap (P2)

- **Ring buffer**: keep up to 64 records while the cloud is down, send them after reconnect
- **Exponential backoff**: 1, 2, 4 … 30 s between reconnect attempts
- **Commands**: cloud → gateway → inverter power limit (`CMD_SET_POWER_LIMIT`), with validation,
  ACK and timeout
- AddressSanitizer / UBSan runs
