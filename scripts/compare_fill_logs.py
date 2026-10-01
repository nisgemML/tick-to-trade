#!/usr/bin/env python3
"""Compare two hft::LogEntry fill logs, ignoring timestamp_ns.

Used to verify FileLogSink and IOURingLogSink produce semantically
identical output for the same input -- timestamp_ns is real wall-clock
time and cannot match between two separate process runs by construction
(same reasoning as quant-signal-research's deterministic-replay test and
mpsc-queue's own node-lifetime test). See BUGS_FOUND.md #12.

Usage: compare_fill_logs.py <log_a> <log_b>
Exit 0 if every field except timestamp_ns matches record-for-record and
both logs have the same record count; exit 1 otherwise.
"""
import struct
import sys

# timestamp_ns, order_id, price, qty, side, event_type, symbol[6], _pad[4]
FMT = "<QQqIcc6s4x"
SIZE = struct.calcsize(FMT)
assert SIZE == 40, f"expected 40-byte LogEntry, got {SIZE}"


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % SIZE != 0:
        sys.exit(f"FAIL: {path} size {len(data)} is not a multiple of {SIZE}")
    return [struct.unpack(FMT, data[i : i + SIZE]) for i in range(0, len(data), SIZE)]


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <log_a> <log_b>")
    a, b = load(sys.argv[1]), load(sys.argv[2])
    if len(a) != len(b):
        sys.exit(f"FAIL: record count differs: {len(a)} vs {len(b)}")
    mismatches = sum(1 for ra, rb in zip(a, b) if ra[1:] != rb[1:])  # [1:] skips timestamp_ns
    print(f"{len(a)} records compared, {mismatches} mismatches (excluding timestamp_ns)")
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
