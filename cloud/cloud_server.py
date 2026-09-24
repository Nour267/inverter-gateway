#!/usr/bin/env python3
"""Cloud server (DESIGN.md §5, §9.2): receives TELEMETRY frames from the gateway over TCP,
checks them with the same state machine and CRC as the C code, and prints live values.

Usage: python3 cloud/cloud_server.py [--port 5000] [--duration N --expect-min M]
  --duration N    stop after N seconds and print a summary (used by scripts/run_demo.sh)
  --expect-min M  with --duration: exit code 1 if fewer than M valid frames or any CRC error
"""
import argparse
import datetime
import select
import socket
import struct
import sys
import time

SOF1, SOF2 = 0xAA, 0x55
PROTO_VERSION = 1
HEADER_LEN = 6                 # after AA 55: version, msg_type, seq (2), payload_len (2)
MAX_PAYLOAD = 256
MSG_TELEMETRY = 0x01
STATE_NAMES = {0: "OFF", 1: "STARTING", 2: "PRODUCING", 3: "FAULT"}


def crc16(data):
    """Goal: CRC-16/CCITT-FALSE, exactly like crc16.c (poly 0x1021, init 0xFFFF).
    In:   data = bytes
    Out:  the 16-bit CRC, e.g. b"123456789" -> 0x29B1"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class Parser:
    """The same state machine as proto_parse_byte() in protocol.c:
    WAIT_SOF1 -> WAIT_SOF2 -> HEADER -> PAYLOAD -> CRC -> frame ready."""

    def __init__(self):
        """Goal: start in WAIT_SOF1 with an empty buffer.
        In:   nothing
        Out:  a new parser"""
        self.reset()

    def reset(self):
        """Goal: go back to the start state (after a frame or an error).
        In:   nothing
        Out:  nothing"""
        self.state = "WAIT_SOF1"
        self.buf = bytearray()      # bytes after AA 55: header, payload, CRC
        self.payload_len = 0

    def feed(self, byte):
        """Goal: feed ONE received byte to the state machine.
        In:   byte = an int 0..255
        Out:  ("frame", (msg_type, seq, payload)), ("error", reason), or None (need more)"""
        if self.state == "WAIT_SOF1":
            if byte == SOF1:
                self.state = "WAIT_SOF2"
            return None

        if self.state == "WAIT_SOF2":
            if byte == SOF2:
                self.state = "HEADER"
                self.buf = bytearray()
            elif byte != SOF1:          # AA AA 55: stay, the second AA may be the start
                self.state = "WAIT_SOF1"
            return None

        self.buf.append(byte)

        if self.state == "HEADER":
            if len(self.buf) < HEADER_LEN:
                return None
            version = self.buf[0]
            self.payload_len = struct.unpack(">H", self.buf[4:6])[0]
            if version != PROTO_VERSION:
                self.reset()
                return ("error", "bad version")
            if self.payload_len > MAX_PAYLOAD:
                self.reset()
                return ("error", "payload too long")
            self.state = "PAYLOAD" if self.payload_len > 0 else "CRC"
            return None

        if self.state == "PAYLOAD":
            if len(self.buf) == HEADER_LEN + self.payload_len:
                self.state = "CRC"
            return None

        # state == "CRC"
        data_len = HEADER_LEN + self.payload_len
        if len(self.buf) < data_len + 2:
            return None
        received = struct.unpack(">H", self.buf[data_len:data_len + 2])[0]
        if received != crc16(self.buf[:data_len]):
            self.reset()
            return ("error", "CRC mismatch")
        msg_type = self.buf[1]
        seq = struct.unpack(">H", self.buf[2:4])[0]
        payload = bytes(self.buf[HEADER_LEN:data_len])
        self.reset()
        return ("frame", (msg_type, seq, payload))


class Stats:
    """Counters for the summary and the integration test."""

    def __init__(self):
        """Goal: all counters start at 0.
        In:   nothing
        Out:  a new Stats"""
        self.frames = 0
        self.crc_errors = 0
        self.other_errors = 0
        self.lost = 0
        self.last_seq = None


def handle_frame(msg_type, seq, payload, stats):
    """Goal: check seq for lost frames, unpack TELEMETRY and print it.
    In:   msg_type, seq, payload = one checked frame; stats = counters to update
    Out:  nothing"""
    stats.frames += 1

    # seq gap = lost frames. "& 0xFFFF" handles the wrap from 65535 to 0.
    if stats.last_seq is not None:
        expected = (stats.last_seq + 1) & 0xFFFF
        gap = (seq - expected) & 0xFFFF
        if gap:
            stats.lost += gap
            print(f"  !! seq jumped {stats.last_seq} -> {seq}: {gap} frame(s) lost")
    stats.last_seq = seq

    if msg_type != MSG_TELEMETRY or len(payload) != 16:
        print(f"  frame seq={seq}: type 0x{msg_type:02X}, {len(payload)} bytes (not TELEMETRY)")
        return

    # Same layout as proto_pack_telemetry: > big-endian, I u32, B u8, h i16 (signed), H u16
    ts, state, fault, temp, dc_v, dc_a, ac_w, ac_v = struct.unpack(">IBBhHHHH", payload)
    when = datetime.datetime.fromtimestamp(ts).strftime("%H:%M:%S")
    fault_text = f"  FAULT code {fault}" if fault else ""
    print(f"seq={seq:<5} {when}  {STATE_NAMES.get(state, state):<9}"
          f" {dc_v / 10:6.1f} V DC  {dc_a / 100:5.2f} A  {ac_w:5d} W"
          f"  {ac_v / 10:5.1f} V AC  {temp / 10:5.1f} C{fault_text}", flush=True)


def print_summary(stats):
    """Goal: print all counters.
    In:   stats
    Out:  nothing"""
    print(f"SUMMARY frames={stats.frames} crc_errors={stats.crc_errors} "
          f"other_errors={stats.other_errors} lost={stats.lost}", flush=True)


def main():
    """Goal: accept one gateway at a time, parse its byte stream, print telemetry.
    In:   command-line options (see the top of this file)
    Out:  exit code 0, or 1 if --expect-min is not met"""
    ap = argparse.ArgumentParser(description="Inverter gateway cloud server")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--duration", type=float, default=None)
    ap.add_argument("--expect-min", type=int, default=None)
    args = ap.parse_args()

    # Same CRC variant as the C code? The standard check value proves it.
    assert crc16(b"123456789") == 0x29B1, "CRC does not match the C implementation"

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)   # restart without "address in use"
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"cloud_server: listening on port {args.port}", flush=True)

    stats = Stats()
    end = time.monotonic() + args.duration if args.duration else None
    conn = None
    parser = Parser()

    try:
        while end is None or time.monotonic() < end:
            # select() = Python's poll(): wait for a new client or data, max 0.5 s
            waiting = [conn] if conn else [srv]
            ready, _, _ = select.select(waiting, [], [], 0.5)
            if not ready:
                continue

            if conn is None:
                conn, addr = srv.accept()
                parser = Parser()
                print(f"gateway connected from {addr[0]}:{addr[1]}", flush=True)
                continue

            data = conn.recv(4096)          # any chunk size: the parser handles it
            if not data:                    # b"" = the gateway closed the connection
                print("gateway disconnected", flush=True)
                conn.close()
                conn = None
                continue

            for byte in data:
                result = parser.feed(byte)
                if result is None:
                    continue
                kind, value = result
                if kind == "frame":
                    handle_frame(*value, stats)
                elif value == "CRC mismatch":
                    stats.crc_errors += 1
                    print("  !! CRC error: frame dropped", flush=True)
                else:
                    stats.other_errors += 1
                    print(f"  !! bad frame: {value}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        if conn:
            conn.close()
        srv.close()
        print_summary(stats)

    if args.expect_min is not None:
        ok = stats.frames >= args.expect_min and stats.crc_errors == 0
        print("RESULT: PASS" if ok else "RESULT: FAIL", flush=True)
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
