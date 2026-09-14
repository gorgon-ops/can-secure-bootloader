#!/usr/bin/env python3
"""Validate the message database and recompute bus load.

Runs without any hardware, so it belongs in CI. Catches the failure mode where
someone adds a frame by hand and quietly pushes the bus past its budget, or
overlaps two signals in the same payload.

    python tools/validate_dbc.py [--max-load 30]
"""
import argparse
import pathlib
import sys

import cantools

DBC = pathlib.Path(__file__).resolve().parent.parent / "dbc" / "bfmc.dbc"
BITRATE = 500_000

# Worst-case bits for a standard CAN 2.0A frame including stuffing and the
# 3-bit interframe space. Conservative by design.
def frame_bits(dlc: int) -> int:
    return 55 + 10 * dlc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-load", type=float, default=30.0,
                    help="fail if steady-state bus load exceeds this percent")
    args = ap.parse_args()

    try:
        # strict=True makes cantools reject overlapping signals and signals
        # that run past the declared DLC, so we do not re-implement those.
        db = cantools.database.load_file(DBC, strict=True)
    except cantools.database.errors.Error as exc:
        print(f"ERROR: {DBC.name} is invalid: {exc}", file=sys.stderr)
        return 1
    except cantools.database.UnsupportedDatabaseFormatError as exc:
        print(f"ERROR: {DBC.name} is invalid: {exc}", file=sys.stderr)
        return 1

    errors = []

    ids = {}
    total_bps = 0.0
    for msg in sorted(db.messages, key=lambda m: m.frame_id):
        if msg.frame_id in ids:
            errors.append(f"duplicate ID 0x{msg.frame_id:03X}: "
                          f"{ids[msg.frame_id]} and {msg.name}")
        ids[msg.frame_id] = msg.name

        if msg.cycle_time:
            total_bps += frame_bits(msg.length) / (msg.cycle_time / 1000.0)

    load = 100.0 * total_bps / BITRATE
    print(f"{len(db.messages)} messages, {len(db.nodes)} nodes")
    print(f"steady-state load: {total_bps:.0f} bit/s = {load:.1f}% of {BITRATE // 1000} kbit/s")

    if load > args.max_load:
        errors.append(f"bus load {load:.1f}% exceeds budget {args.max_load:.1f}%")

    for err in errors:
        print(f"ERROR: {err}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
