#!/usr/bin/env python3
"""Build and run every firmware host test with the system C compiler.

Run from firmware/esp32s3:  python tests/run_host_tests.py [--cc gcc]

Each test is plain C11 compiled with -Wall -Wextra -Werror. Lines a test
prints as "JSON:<document>" are parsed with Python's json module, so the
telemetry builder is checked by a real parser, not only substrings.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTS = {
    "threshold_monitor": (["components/threshold_monitor/include"],
                          ["components/threshold_monitor/threshold_monitor.c"]),
    "pump_battery": (["components/irrigation/include", "components/battery_monitor/include"],
                     ["components/irrigation/irrigation.c",
                      "components/battery_monitor/battery_policy.c"]),
    "record_log": (["components/record_log/include"],
                   ["components/record_log/record_log.c"]),
    "status_led": (["components/status_led/include"],
                   ["components/status_led/status_led_policy.c"]),
    "data_manager": (["tests/stubs", "components/data_manager/include",
                      "components/sensor_manager/include", "components/bh1750/include",
                      "components/dht20/include", "components/soil_moisture/include"],
                     ["components/data_manager/data_manager.c"]),
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("gcc") or "gcc")
    args = parser.parse_args()
    failures = 0
    with tempfile.TemporaryDirectory() as folder:
        for name, (includes, sources) in TESTS.items():
            binary = Path(folder) / f"test_{name}"
            # gmtime_r (data_manager) is POSIX, not ISO C11.
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-D_POSIX_C_SOURCE=200809L", "-D_POSIX_THREAD_SAFE_FUNCTIONS",
                       *[f"-I{ROOT / path}" for path in includes],
                       str(ROOT / "tests" / f"test_{name}.c"),
                       *[str(ROOT / path) for path in sources], "-lm", "-o", str(binary)]
            build = subprocess.run(command, capture_output=True, text=True)
            if build.returncode != 0:
                print(f"FAIL {name}: build\n{build.stderr}")
                failures += 1
                continue
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            documents = 0
            for line in run.stdout.splitlines():
                if line.startswith("JSON:"):
                    json.loads(line[5:])
                    documents += 1
            summary = [line for line in run.stdout.splitlines() if not line.startswith("JSON:")]
            status = "PASS" if run.returncode == 0 else "FAIL"
            failures += run.returncode != 0
            print(f"{status} {name}: {'; '.join(summary)}"
                  + (f" ({documents} JSON documents parsed)" if documents else ""))
            if run.returncode != 0:
                print(run.stderr)
    print("All host tests passed" if not failures else f"{failures} host test(s) failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
