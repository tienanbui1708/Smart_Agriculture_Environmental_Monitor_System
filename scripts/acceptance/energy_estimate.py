#!/usr/bin/env python3
"""Average current and battery life from measured phase currents.

Example (Battery mode, 300 s interval: awake 12 s at 95 mA, asleep the rest
at 0.15 mA; 2000 mAh cell, 80 % usable):

    python scripts/acceptance/energy_estimate.py --interval 300 \\
        --phase awake:12:95 --sleep-ma 0.15 --capacity-mah 2000

Phases are name:seconds:milliamps per sampling cycle; the remainder of the
interval is spent at --sleep-ma. Fill the measured values from
docs/energy-measurement.md.
"""
from __future__ import annotations

import argparse
import sys


def average_current(interval_s: float, phases: list[tuple[str, float, float]],
                    sleep_ma: float) -> float:
    active_s = sum(seconds for _, seconds, _ in phases)
    if active_s > interval_s:
        raise ValueError("phases are longer than the interval")
    charge = sum(seconds * milliamps for _, seconds, milliamps in phases)
    charge += (interval_s - active_s) * sleep_ma
    return charge / interval_s


def battery_hours(capacity_mah: float, usable: float, average_ma: float) -> float:
    return capacity_mah * usable / average_ma


def parse_phase(text: str) -> tuple[str, float, float]:
    name, seconds, milliamps = text.split(":")
    return name, float(seconds), float(milliamps)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--interval", type=float, required=True, help="sampling interval, s")
    parser.add_argument("--phase", action="append", type=parse_phase, default=[],
                        help="name:seconds:mA, repeatable")
    parser.add_argument("--sleep-ma", type=float, default=0.0)
    parser.add_argument("--capacity-mah", type=float, default=2000.0)
    parser.add_argument("--usable", type=float, default=0.8, help="usable fraction of capacity")
    args = parser.parse_args()
    average = average_current(args.interval, args.phase, args.sleep_ma)
    hours = battery_hours(args.capacity_mah, args.usable, average)
    for name, seconds, milliamps in args.phase:
        print(f"{name:>10}: {seconds:g} s @ {milliamps:g} mA")
    print(f"{'sleep':>10}: {args.interval - sum(p[1] for p in args.phase):g} s @ {args.sleep_ma:g} mA")
    print(f"Average current: {average:.3f} mA")
    print(f"Battery life ({args.capacity_mah:g} mAh x {args.usable:.0%}): "
          f"{hours:.0f} h = {hours / 24:.1f} days")
    return 0


if __name__ == "__main__":
    sys.exit(main())
