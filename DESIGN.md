# Inverter Gateway — Design Document

> A small embedded Linux project: a gateway, written in C, that reads data from a (simulated)
> solar inverter over CAN bus and sends it to a "cloud" server over TCP, using a custom
> binary protocol.

**Status:** Draft v0.1 — planning
**Author:** Noor Abu Elfoul

---

## 0. How to read this document

Sections are tagged with the phase they belong to:

- **[P1]**: short version, built first (about 5–6 days)
- **[P2]**: full version, added on top of P1 (about 4 more days)

Anything not tagged applies to both phases. Every design decision includes a **Why**, because
"why did you do it this way?" is the most common interview question about a project.

---

## 1. Goal

Build a small version of what a device "Interface" team builds: the software on a device that
connects the hardware to the outside world.

**The project must show:**
1. C code for a Linux target that handles bytes, structs and bit-level data correctly
2. Talking to "hardware" over a real embedded bus (CAN, via Linux SocketCAN)
3. A reliable device-to-cloud link: framing, checksums, reconnects
4. Unit tests for the low-level logic
5. Defensive handling of bad input (never trust data from the bus or the network)

**Out of scope (listed on purpose, so the scope stays small):**
- Real hardware (everything is simulated on one Linux machine)
- TLS or encryption, authentication, firmware updates
- A GUI or a database
- Multi-threading (see §6.1 for why)

---

## 2. System overview

```
 ┌────────────────────┐   CAN frames     ┌─────────────────────────┐   TCP + custom protocol   ┌──────────────────┐
 │  inverter_sim (C)  │ ───────────────▶ │     gateway (C)         │ ────────────────────────▶ │ cloud_server.py  │
 │                    │   (vcan0)        │                         │                           │   (Python)       │
 │ - fakes a solar    │                  │ - reads & decodes CAN   │                           │ - parses frames  │
 │   inverter         │ ◀─────────────── │ - validates values      │ ◀──────────────────────── │ - checks CRC     │
 │ - sends telemetry  │  [P2] commands   │ - encodes uplink frames │   [P2] commands           │ - prints data    │
 │ - [P2] obeys       │                  │ - reconnects to cloud   │                           │ - [P2] sends     │
 │   power limit      │                  │ - [P2] ring buffer      │                           │   commands       │
 └────────────────────┘                  └─────────────────────────┘                           └──────────────────┘
      "the hardware"                          "the device software"                                "the cloud"
                                              ← THE MAIN PART →
```

All three programs run on the same Linux machine (WSL2 on Windows).

| Component | Language | Why this language |
|---|---|---|
| `inverter_sim` | C | Uses the same CAN code as the gateway |
| `gateway` | C | The embedded part, and what the job asks for |
| `cloud_server.py` | Python | Fast to write, and the posting lists Python as an advantage. It also proves the protocol works across languages. |

---

## 3. Hardware abstraction: the bus layer

The gateway never calls SocketCAN directly. It calls a small **bus interface**:

```c
/* include/bus.h */
typedef struct {
    uint32_t id;       /* 11-bit standard CAN ID */
    uint8_t  len;      /* 0..8 data bytes */
    uint8_t  data[8];
} bus_frame_t;

int  bus_open(const char *ifname);                   /* 0 on success, -1 on error */
int  bus_send(const bus_frame_t *frame);             /* 0 on success, -1 on error */
int  bus_recv(bus_frame_t *frame);                   /* 1 = got frame, 0 = none, -1 = error */
int  bus_fd(void);                                   /* file descriptor, for poll() */
void bus_close(void);
```

There are two implementations, chosen at build time (`make BUS=socketcan` or `make BUS=udp`):

| Backend | File | When to use it |
|---|---|---|
| SocketCAN | `src/bus/bus_socketcan.c` | Default. Uses Linux virtual CAN (`vcan0`). |
| UDP | `src/bus/bus_udp.c` | Fallback, if the WSL2 kernel has no `vcan` module |

**Why:** This is a **HAL (Hardware Abstraction Layer)**. The rest of the gateway doesn't know,
or care, whether frames come from a real CAN chip, a virtual CAN bus, or UDP. On a real
product, you would port the device by writing a new `bus_*.c` file, and nothing else changes.

---

## 4. CAN message map (inverter ↔ gateway)

**Conventions:**
- Standard 11-bit IDs, always 8 data bytes (DLC = 8)
- Multi-byte values are **little-endian** (least significant byte first)
- Physical values are sent as **scaled integers**, never floats

**Why scaled integers:** Many small microcontrollers have no floating-point hardware, and
integers have an exact, fixed size on the wire. Example: 230.5 V is sent as `2305` in units
of 0.1 V.

### 0x100 `INVERTER_STATUS`: inverter → gateway, every 1000 ms [P1]

| Byte | Field | Type | Unit | Valid range |
|---|---|---|---|---|
| 0 | `state` | u8 | enum: 0=OFF, 1=STARTING, 2=PRODUCING, 3=FAULT | 0..3 |
| 1 | `fault_code` | u8 | 0 = no fault | 0..255 |
| 2–3 | `temperature` | i16 | 0.1 °C | -400..1500 (-40.0..150.0 °C) |
| 4–7 | `uptime` | u32 | seconds | any |

### 0x101 `INVERTER_POWER`: inverter → gateway, every 1000 ms [P1]

| Byte | Field | Type | Unit | Valid range |
|---|---|---|---|---|
| 0–1 | `dc_voltage` | u16 | 0.1 V | 0..10000 (0..1000 V) |
| 2–3 | `dc_current` | u16 | 0.01 A | 0..5000 (0..50 A) |
| 4–5 | `ac_power` | u16 | 1 W | 0..20000 |
| 6–7 | `ac_voltage` | u16 | 0.1 V | 0..3000 (0..300 V) |

### 0x200 `CMD_SET_POWER_LIMIT`: gateway → inverter [P2]

| Byte | Field | Type | Unit | Valid range |
|---|---|---|---|---|
| 0 | `limit_pct` | u8 | % of max power | 0..100 |
| 1–7 | reserved | — | set to 0 | — |

### 0x201 `CMD_ACK`: inverter → gateway [P2]

| Byte | Field | Type | Meaning |
|---|---|---|---|
| 0 | `result` | u8 | 0 = OK, 1 = REJECTED |
| 1 | `applied_limit_pct` | u8 | The limit now in effect |
| 2–7 | reserved | — | 0 |

**Decoding rules (gateway):**
- Wrong length (DLC ≠ 8): drop the frame and increment `can_bad_len`
- Unknown ID: ignore the frame and increment `can_unknown_id`
- Value out of range: drop the frame, increment `can_out_of_range`, and log a warning

---

## 5. Uplink protocol (gateway ↔ cloud, over TCP)

### 5.1 Frame format

```
┌──────┬──────┬─────────┬──────────┬─────────┬─────────────┬───────────────┬─────────┐
│ 0xAA │ 0x55 │ version │ msg_type │   seq   │ payload_len │   payload     │  CRC16  │
│  1B  │  1B  │   1B    │    1B    │   2B    │     2B      │  0..256 B     │   2B    │
└──────┴──────┴─────────┴──────────┴─────────┴─────────────┴───────────────┴─────────┘
         SOF                   ↑─────────────── CRC covers these bytes ───────────────↑
```

- **Byte order:** big-endian ("network byte order"), the usual convention for network protocols
- **version:** `1`. A frame with an unknown version is rejected.
- **seq:** increases by 1 for each frame sent and wraps from 65535 back to 0. The server uses it
  to detect lost frames.
- **payload_len:** at most 256. Anything larger is rejected **before** any byte is copied.
- **CRC:** CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`), calculated over
  `version .. end of payload`. Test value: CRC of ASCII `"123456789"` = `0x29B1`.
- **Overhead:** 10 bytes per frame

**Why length-prefixed framing:** TCP is a *byte stream*, not a message stream. One `recv()`
can return half a frame, or one and a half frames. The length field tells the receiver exactly
where each frame ends.

**Why SOF (start-of-frame) bytes and a CRC, when TCP already has a checksum:**
1. They catch **our own bugs** (a wrong length, a bad pack/unpack), not just line noise.
2. The same protocol could later run over UART or cellular modems, which have no such
   protection.
3. After a bad frame, the parser can **resynchronize** by scanning for `0xAA 0x55`.
   (This is an honest interview answer: on pure TCP the CRC is mainly defense in depth.)

**Why pack byte by byte with shifts instead of sending a C struct with `memcpy`:** The compiler
may add **padding** inside structs, and the CPU's byte order may differ from the protocol's.
Explicit packing gives the exact same bytes on every machine.

### 5.2 Message types

| Type | Name | Direction | Payload | Phase |
|---|---|---|---|---|
| `0x01` | `TELEMETRY` | gw → cloud | see below, 16 bytes | P1 |
| `0x10` | `CMD_SET_POWER_LIMIT` | cloud → gw | `cmd_id` u16, `limit_pct` u8 | P2 |
| `0x11` | `CMD_RESULT` | gw → cloud | `cmd_id` u16, `result` u8 (0=OK, 1=REJECTED, 2=INVALID, 3=TIMEOUT) | P2 |

`TELEMETRY` payload (16 bytes):

| Offset | Field | Type |
|---|---|---|
| 0 | `timestamp` (Unix seconds) | u32 |
| 4 | `state` | u8 |
| 5 | `fault_code` | u8 |
| 6 | `temperature` (0.1 °C) | i16 |
| 8 | `dc_voltage` (0.1 V) | u16 |
| 10 | `dc_current` (0.01 A) | u16 |
| 12 | `ac_power` (W) | u16 |
| 14 | `ac_voltage` (0.1 V) | u16 |

### 5.3 Receiving: the parser state machine

The parser is fed **one byte at a time** and never assumes a whole frame arrives at once:

```
          byte == 0xAA            byte == 0x55            6 header bytes
 WAIT_SOF1 ──────────▶ WAIT_SOF2 ──────────▶ HEADER ──────────────▶ PAYLOAD ──▶ CRC ──▶ frame ready
     ▲                     │ other                │ bad version or      (payload_len    │
     └─────────────────────┘                      │ payload_len > 256   bytes)          │ CRC mismatch
     ▲                                            │                                     │
     └────────────────────────────────────────────┴─────────────────────────────────────┘
                                   error: count it, go back to WAIT_SOF1
```

**Why a state machine:** It handles partial reads, garbage bytes and corrupted frames in one
simple, testable piece of code, with no dynamic memory. The same parser code runs on both
ends; the Python server has its own version of it.

---

## 6. Gateway internals

### 6.1 Main loop: single-threaded, event-driven

```c
while (running) {
    poll({bus_fd, tcp_fd}, timeout = 100 ms);

    if (bus readable)   handle_can_frames();     /* decode + validate + store latest values */
    if (tcp readable)   handle_uplink_rx();      /* [P2] commands from cloud */
    if (1 second passed) send_telemetry();       /* combine latest STATUS + POWER, send */
    manage_connection();                         /* reconnect if needed */
    check_timeouts();                            /* stale CAN data, [P2] command timeout */
}
```

**Why one thread with `poll()`, and not threads:** No locks, no race conditions, and easy to
reason about and debug. Many embedded systems use this "super loop" or event-loop design.
`poll()` sleeps until something happens, so the loop doesn't waste CPU.

### 6.2 Combining data

- The gateway keeps the **latest** `INVERTER_STATUS` and `INVERTER_POWER`, each with the time
  it was received.
- Once per second it sends one `TELEMETRY` frame built from both.
- **Stale data:** if either message is older than 3 s, the gateway logs "inverter not
  responding" and **does not send** telemetry. Old data is never presented as new.

### 6.3 Connection management

```
 DISCONNECTED ──try connect──▶ CONNECTED
      ▲                            │
      └── send/recv error, or ─────┘
          peer closed (recv == 0)
```

- **[P1]** Retry every 2 seconds.
- **[P2]** Exponential backoff: 1, 2, 4, 8, 16, 30, 30, ... seconds, reset to 1 after a
  successful connect.
  **Why:** If the cloud is down, a million devices retrying every second would overload it
  when it comes back.
- Ignore `SIGPIPE`, so writing to a closed socket returns an error instead of killing the
  process.
- *Known P1 limitation:* `connect()` is blocking. To localhost this is instant; a real product
  would use non-blocking connect. Mention this if asked, since knowing your limits looks good.

### 6.4 Ring buffer [P2]

When the cloud is unreachable, telemetry is stored instead of thrown away:

```
 capacity = 64 (power of 2)        head → next write position
 ┌───┬───┬───┬───┬───┬───┬───┬───┐ tail → next read position
 │ T │ T │ T │   │   │   │ T │ T │ full → overwrite oldest, dropped++
 └───┴───┴───┴───┴───┴───┴───┴───┘
           ↑head           ↑tail
```

- Fixed-size, statically allocated array of telemetry records (no `malloc`)
- When full, **overwrite the oldest** entry. Recent data is worth more than old data.
  Count every dropped record.
- On reconnect, flush the buffered records in order, then continue live
- A power-of-2 capacity allows `index & (CAP - 1)` instead of `%`

### 6.5 Command flow [P2]

```
cloud ──CMD_SET_POWER_LIMIT(id=7, 50%)──▶ gateway ──validate 0..100──▶ CAN 0x200 ──▶ inverter
cloud ◀──CMD_RESULT(id=7, OK)──────────── gateway ◀──────────────────── CAN 0x201 ◀── inverter
```

- The gateway **validates before forwarding**. A limit > 100 gets `INVALID` and never reaches
  the inverter. **Why:** never trust input from the network.
- Only **one command pending** at a time; a second one while busy gets `REJECTED`
- No `CMD_ACK` within 2 s → `TIMEOUT`

### 6.6 Memory, errors and logging

- **No `malloc` after startup.** All buffers are fixed-size. **Why:** Embedded systems avoid
  heap fragmentation and out-of-memory at runtime, so memory use is known in advance.
- Every function that can fail returns an error code, and callers check it.
- Logging to stderr: `[time] [LEVEL] message`, levels ERROR / WARN / INFO / DEBUG
- **Counters** (printed every 10 s and on exit): `can_rx`, `can_bad_len`, `can_unknown_id`,
  `can_out_of_range`, `tx_frames`, `reconnects`, [P2] `buffered`, `dropped`, `cmds`
- `SIGINT`/`SIGTERM` set `running = 0` for a clean shutdown (close sockets, print counters)

### 6.7 Command-line options

```
gateway   --bus vcan0 --server 127.0.0.1:5000 [--verbose]
inverter_sim --bus vcan0 [--fault-at SEC] [--bad-frames]
cloud_server.py --port 5000
```

---

## 7. Inverter simulator

- **Sun curve:** `ac_power` follows a sine wave over a shortened "day" (1 day = 2 minutes),
  with small random noise
- **Temperature** rises with power
- **State:** OFF at night, then STARTING, then PRODUCING
- `--fault-at N`: after N seconds, go to FAULT with a fault code (tests fault reporting)
- `--bad-frames`: sometimes send a wrong DLC or out-of-range value (tests gateway validation)
- **[P2]** Obeys `CMD_SET_POWER_LIMIT`: `ac_power` is capped at `limit_pct` of maximum, and it
  replies with `CMD_ACK`

---

## 8. Security considerations

Kept small but deliberate, because the posting lists embedded security as an advantage:

| Risk | Mitigation |
|---|---|
| Buffer overflow from a malicious `payload_len` | Check `payload_len ≤ 256` **before** copying anything |
| Garbage or corrupted input | SOF + CRC + version check; the parser resyncs |
| Invalid commands reaching hardware | Range-check every command field in the gateway |
| Crashing on a closed connection | Ignore `SIGPIPE`; handle `recv() == 0` |
| Unsafe C functions | No `strcpy`/`sprintf`/`gets`; use `snprintf` and explicit lengths |
| Compiler-catchable bugs | Build with `-Wall -Wextra -Werror`; run tests with `-fsanitize=address,undefined` |
| **Not covered** (future work) | TLS, device authentication, signed commands. Name them in the interview. |

---

## 9. Testing strategy

### 9.1 Unit tests (C, [Unity](https://github.com/ThrowTheSwitch/Unity) framework)

Unity is a small C test framework widely used in embedded projects. It's vendored in `tests/unity/`.

| Test file | What it checks |
|---|---|
| `test_crc16.c` | Known test value `"123456789"` → `0x29B1`; empty input |
| `test_protocol.c` | Pack → parse round trip; frame split into single bytes; garbage before SOF; bad CRC rejected; `payload_len` 257 rejected; `seq` wraparound |
| `test_can_messages.c` | Decoding with correct scaling; negative temperature; wrong DLC; out-of-range values |
| `test_ringbuf.c` [P2] | Push/pop order; wraparound; overwrite when full; dropped count |

Run with `make test`. Tests don't need CAN or the network, because they test **pure logic**.
This is the benefit of keeping protocol code separate from I/O code.

### 9.2 Integration test (`scripts/run_demo.sh` / Python)

1. Start the server, the gateway and the simulator
2. After 10 s, check that the server received ≥ 8 valid telemetry frames and 0 CRC errors
3. **[P2]** Stop the server for 10 s, restart it, and check that the buffered frames arrive
   (no `seq` gaps)
4. **[P2]** Send a power limit command, and check `CMD_RESULT = OK` and that `ac_power` drops

### 9.3 Manual debugging tools

- `candump vcan0` (from `can-utils`) shows raw CAN frames
- `cansend vcan0 100#0200F401...` injects a hand-made frame

---

## 10. Project layout

```
inverter-gateway/
├── DESIGN.md
├── README.md                 ← how to build/run + diagram (the "interview script")
├── Makefile
├── include/
│   ├── bus.h                 ← HAL interface
│   ├── can_messages.h        ← CAN IDs, structs, decode/encode
│   ├── protocol.h            ← uplink frame pack/parse
│   ├── crc16.h
│   ├── log.h
│   └── ringbuf.h             [P2]
├── src/
│   ├── common/               ← pure logic, unit-tested, no I/O
│   │   ├── crc16.c
│   │   ├── protocol.c
│   │   ├── can_messages.c
│   │   ├── log.c
│   │   └── ringbuf.c         [P2]
│   ├── bus/
│   │   ├── bus_socketcan.c
│   │   └── bus_udp.c
│   ├── gateway/
│   │   ├── main.c            ← main loop
│   │   └── uplink.c          ← TCP connection handling
│   └── sim/
│       └── inverter_sim.c
├── cloud/
│   └── cloud_server.py
├── tests/
│   ├── unity/
│   ├── test_crc16.c
│   ├── test_protocol.c
│   ├── test_can_messages.c
│   └── test_ringbuf.c        [P2]
└── scripts/
    ├── setup_vcan.sh         ← modprobe vcan; ip link add vcan0 type vcan; ip link set up vcan0
    └── run_demo.sh
```

**Key rule:** `src/common/` does **no I/O** (no sockets, no printing except through `log`). Code
that doesn't touch hardware or the network can be unit-tested anywhere.

---

## 11. Milestones

Each milestone ends with a git commit and **you explaining it in your own words**.

| # | Milestone | Done when | Phase |
|---|---|---|---|
| M0 | Environment | WSL2 + gcc + make + git + can-utils work; `vcan0` is up (or the UDP fallback is chosen) | P1 |
| M1 | CAN message decoding | `test_can_messages` passes | P1 |
| M2 | CRC + protocol pack/parse | All `test_crc16` and `test_protocol` tests pass | P1 |
| M3 | Bus layer + simulator | `candump vcan0` shows the simulator's frames | P1 |
| M4 | Gateway: CAN → TCP | Gateway reads CAN, sends TELEMETRY; reconnects every 2 s | P1 |
| M5 | Cloud server + demo | Server prints live data; integration test passes; README written | **P1 done** |
| M6 | Ring buffer + backoff | Stop/restart the server → no data lost (up to 64 records) | P2 |
| M7 | Commands | Power limit round trip works, including INVALID/TIMEOUT cases | P2 |
| M8 | Polish + mock interview | Sanitizers clean; you can answer the questions in §12 | **P2 done** |

---

## 12. Questions this project should prepare you for

- Walk me through what happens to one temperature reading, from the inverter to the cloud.
- Why did you use scaled integers instead of floats?
- What is endianness? Where does your project deal with it?
- Why not just `send()` a C struct?
- TCP already guarantees delivery. Why do you need length, SOF and CRC?
- What happens if `recv()` returns only half a frame?
- What happens when the cloud goes down for a minute? For an hour? *(P2: ring buffer size and overwrite policy)*
- Why single-threaded? When would you use threads?
- What is a HAL? How would you port this to real hardware?
- How do you protect against a malicious or broken packet?
- Why no `malloc`?
- How did you test code that talks to hardware you don't have?
- How did you use AI tools while building this, and how did you verify what they produced?

---

## 13. Glossary

| Term | Meaning |
|---|---|
| **CAN bus** | A two-wire bus used in cars and industrial devices. Messages have an ID and up to 8 data bytes. |
| **SocketCAN** | Linux's way of using CAN: a CAN bus looks like a network socket |
| **vcan** | Virtual CAN interface: a CAN bus in software, for testing without hardware |
| **DLC** | Data Length Code: how many data bytes a CAN frame has (0–8) |
| **Endianness** | The order of bytes in a multi-byte number. Little-endian puts the low byte first; big-endian puts the high byte first. |
| **Framing** | Marking where each message starts and ends in a byte stream |
| **CRC** | Cyclic Redundancy Check: a short value calculated from the data to detect corruption |
| **HAL** | Hardware Abstraction Layer: an interface that hides hardware details from the rest of the code |
| **Ring buffer** | A fixed-size array used as a queue, where positions wrap around to the start |
| **Exponential backoff** | Waiting longer after each failed retry (1 s, 2 s, 4 s ...) |
| **Sanitizers** | Compiler options (`-fsanitize=...`) that detect memory bugs while the program runs |
