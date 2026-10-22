#!/usr/bin/env python3
"""Cross-check the generated C encoder against cantools reading the DBC.

A round-trip cannot catch a wrong bit offset or endianness: pack and unpack are
wrong the same way. Two independent encoders only agree if the layout is right.

python tests/cross_check.py [--cc gcc]
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

import cantools

ROOT = pathlib.Path(__file__).resolve().parent.parent
DBC = ROOT / "dbc" / "bfmc.dbc"
GENERATED = ROOT / "firmware" / "common" / "protocol" / "generated"
TEST_SRC = ROOT / "tests" / "test_protocol.c"

# Raw values, kept in step with dump_vectors() in test_protocol.c.
VECTORS = {
    "MOTION_CMD": {
        "TargetSpeed": -1500,
        "TargetSteerAngle": -123,
        "CmdCounter": 200,
    },
    "MOTION_STATUS": {
        "MeasSpeed": -874,
        "MeasSteerAngle": 231,
        "MotorCurrent": -12345,
        "MotionFlags": 0xBEEF,
    },
    "IMU_GYRO": {
        "GyroX": -2000,
        "GyroY": 0,
        "GyroZ": 1999,
    },
    "BATTERY": {
        "PackVoltage": 12345,
        "PackCurrent": -6789,
        "StateOfCharge": 87,
        "BatteryFlags": 0x5A,
    },
    "WHEEL_ENCODER": {
        "EncoderTicks": -123456789,
        "DistanceTravelled": 987654321,
    },
}


def build_and_dump(cc: str) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / "dump_vectors"
        cmd = [
            cc, "-std=c99", "-Wall", "-Wextra",
            f"-I{GENERATED}",
            str(TEST_SRC), str(GENERATED / "bfmc.c"),
            "-lm", "-o", str(exe),
        ]
        subprocess.run(cmd, check=True)
        out = subprocess.run([str(exe), "--dump-vectors"],
                             check=True, capture_output=True, text=True).stdout

    encodings = {}
    for line in out.splitlines():
        if not line.strip():
            continue
        name, hexbytes = line.split()
        encodings[name] = hexbytes.upper()
    return encodings


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="gcc", help="host C compiler")
    args = ap.parse_args()

    if shutil.which(args.cc) is None:
        print(f"ERROR: compiler {args.cc!r} not found", file=sys.stderr)
        return 1

    db = cantools.database.load_file(DBC, strict=True)
    from_c = build_and_dump(args.cc)

    failures = 0
    for name, signals in VECTORS.items():
        msg = db.get_message_by_name(name)

        # Raw units on both sides keeps float rounding out of the comparison.
        expected = msg.encode(signals, scaling=False).hex().upper()
        actual = from_c.get(name)

        if actual is None:
            print(f"FAIL {name}: the C program emitted no vector", file=sys.stderr)
            failures += 1
        elif actual != expected:
            print(f"FAIL {name}: C={actual} cantools={expected}", file=sys.stderr)
            failures += 1
        else:
            print(f"ok   {name} {actual}")

    extra = set(from_c) - set(VECTORS)
    for name in sorted(extra):
        print(f"FAIL {name}: emitted by C but not covered here", file=sys.stderr)
        failures += 1

    print(f"{len(VECTORS)} vectors, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
