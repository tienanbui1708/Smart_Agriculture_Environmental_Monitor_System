#!/usr/bin/env python3
"""Stage 4 acceptance test (PRD "Thử nghiệm Edge-case & Resilience").

Scenario from the specification:
  1. Stop the mini server (MQTT broker)      -> device keeps sampling offline
  2. Phone connects directly (BLE / Wi-Fi AP) and shows live data
  3. Start the server again                  -> device re-syncs the backlog
The script drives the steps, collects evidence and writes a Markdown report
with a PASS/FAIL verdict based on `seq` continuity.

Usage (Python 3.10+, standard library only):

  # guided run, you stop/start the broker yourself when asked
  python scripts/acceptance/stage4_resilience.py --offline-minutes 5

  # let the script stop/start saems-mqtt over SSH (asks for sudo on the Pi)
  python scripts/acceptance/stage4_resilience.py --ssh admin@100.104.200.110

  # only analyse an existing time window (UTC ISO times)
  python scripts/acceptance/stage4_resilience.py --analyze-only \\
      --from 2026-12-01T08:00:00Z --to 2026-12-01T09:00:00Z

Credentials: SAEMS_API_TOKEN (bearer, session or API token) or an interactive
login (user/password -> /auth/login). For the optional AP evidence step set
SAEMS_DEVICE_TOKEN (default: the AP Wi-Fi password) and join the device AP.
Nothing secret is written to the report.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
import getpass
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_API = "https://admin.tailb1f0ba.ts.net"
AP_STATUS = "http://192.168.4.1/v1/status"


# ---- analysis (pure, unit tested) -------------------------------------------

@dataclass
class BootSummary:
    boot_id: str
    first_seq: int
    last_seq: int
    received: int
    missing: list[int] = field(default_factory=list)
    duplicates: int = 0


@dataclass
class Analysis:
    total: int
    boots: list[BootSummary]
    backlog: int              # records sampled during the outage, delivered after it
    max_delivery_delay_s: float
    missing_total: int

    @property
    def continuous(self) -> bool:
        return self.missing_total == 0 and all(boot.duplicates == 0 for boot in self.boots)


def parse_time(value) -> datetime | None:
    if not value:
        return None
    try:
        parsed = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=timezone.utc)


def analyze(records: list[dict], outage_start: datetime | None = None,
            outage_end: datetime | None = None) -> Analysis:
    """seq continuity per boot_id; seq restarts only with a new boot_id."""
    per_boot: dict[str, list[int]] = {}
    backlog = 0
    max_delay = 0.0
    for record in records:
        boot = str(record.get("boot_id"))
        seq = record.get("seq")
        if seq is None:
            continue
        per_boot.setdefault(boot, []).append(int(seq))
        sampled = parse_time(record.get("timestamp"))
        received = parse_time(record.get("received_at"))
        if sampled and received:
            max_delay = max(max_delay, (received - sampled).total_seconds())
            if (outage_start and outage_end and outage_start <= sampled <= outage_end
                    and received > outage_end):
                backlog += 1
    boots = []
    for boot, values in per_boot.items():
        unique = sorted(set(values))
        expected = set(range(unique[0], unique[-1] + 1))
        boots.append(BootSummary(
            boot_id=boot, first_seq=unique[0], last_seq=unique[-1],
            received=len(unique), missing=sorted(expected - set(unique)),
            duplicates=len(values) - len(unique)))
    boots.sort(key=lambda summary: summary.first_seq)
    return Analysis(total=len(records), boots=boots, backlog=backlog,
                    max_delivery_delay_s=max_delay,
                    missing_total=sum(len(boot.missing) for boot in boots))


def compress(numbers: list[int]) -> str:
    """[1,2,3,7,9,10] -> '1-3, 7, 9-10'."""
    if not numbers:
        return "-"
    ranges, start, previous = [], numbers[0], numbers[0]
    for number in numbers[1:] + [None]:
        if number is not None and number == previous + 1:
            previous = number
            continue
        ranges.append(f"{start}" if start == previous else f"{start}-{previous}")
        if number is not None:
            start = previous = number
    return ", ".join(ranges)


# ---- I/O ------------------------------------------------------------------

class Api:
    def __init__(self, base: str, token: str):
        self.base = base.rstrip("/")
        self.token = token

    def get(self, path: str, query: dict | None = None):
        url = self.base + path + ("?" + urllib.parse.urlencode(query) if query else "")
        request = urllib.request.Request(url, headers={"Authorization": f"Bearer {self.token}"})
        with urllib.request.urlopen(request, timeout=15) as response:
            return json.loads(response.read().decode("utf-8"))

    def telemetry(self, device: str, start: datetime, end: datetime) -> list[dict]:
        """All records whose timestamp lies in [start, end], paging backwards."""
        records: dict[tuple, dict] = {}
        upper = end
        while True:
            items = self.get(f"/api/v1/devices/{device}/telemetry", {
                "limit": 1000, "from": start.isoformat(), "to": upper.isoformat()})["items"]
            for item in items:
                records[(item.get("boot_id"), item.get("seq"), item.get("timestamp"))] = item
            if len(items) < 1000:
                break
            oldest = parse_time(items[0].get("timestamp"))
            if oldest is None or oldest >= upper:
                break
            upper = oldest
        return sorted(records.values(), key=lambda item: str(item.get("received_at")))


def login(base: str) -> str:
    token = os.environ.get("SAEMS_API_TOKEN")
    if token:
        return token
    user = input("Server user [admin]: ").strip() or "admin"
    password = getpass.getpass("Password: ")
    request = urllib.request.Request(
        base.rstrip("/") + "/auth/login",
        data=json.dumps({"username": user, "password": password}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(request, timeout=15) as response:
        return json.loads(response.read())["access_token"]


def ap_status() -> dict | None:
    token = os.environ.get("SAEMS_DEVICE_TOKEN")
    if not token:
        return None
    try:
        request = urllib.request.Request(AP_STATUS, headers={"Authorization": f"Bearer {token}"})
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.loads(response.read())
    except (urllib.error.URLError, OSError, ValueError):
        return None


def broker(action: str, ssh: str | None) -> None:
    if ssh:
        print(f"-> ssh {ssh}: sudo systemctl {action} saems-mqtt")
        subprocess.run(["ssh", "-t", ssh, f"sudo systemctl {action} saems-mqtt"], check=True)
    else:
        input(f"Hãy {'TẮT' if action == 'stop' else 'BẬT'} server (saems-mqtt trên Pi) rồi nhấn Enter... ")


def wait(minutes: float, message: str) -> None:
    end = time.monotonic() + minutes * 60
    while (remaining := end - time.monotonic()) > 0:
        print(f"\r{message}: còn {int(remaining // 60):02d}:{int(remaining % 60):02d}", end="", flush=True)
        time.sleep(min(5, remaining))
    print()


def yes(question: str) -> bool:
    return input(f"{question} [y/N]: ").strip().lower() in ("y", "yes", "c", "co", "có")


# ---- report -----------------------------------------------------------------

def report(path: Path, device: str, analysis: Analysis, events: dict, notes: list[str]) -> bool:
    phone_ok = events.get("phone_ok", None)
    synced = events.get("synced", True)
    passed = analysis.continuous and synced and phone_ok is not False and analysis.total > 0
    lines = [
        "# Báo cáo nghiệm thu Giai đoạn 4 — Resilience",
        "",
        f"- Thiết bị: `{device}`",
        f"- Tạo lúc: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}",
        f"- Kết luận: **{'ĐẠT' if passed else 'KHÔNG ĐẠT'}**",
        "",
        "## Diễn biến",
        "",
        "| Bước | Thời điểm (UTC) |",
        "|---|---|",
    ]
    for key, label in (("start", "Bắt đầu"), ("stopped", "Tắt server"),
                       ("phone", "Điện thoại kết nối trực tiếp"), ("restarted", "Bật lại server"),
                       ("synced_at", "Đồng bộ xong")):
        if key in events and isinstance(events[key], datetime):
            lines.append(f"| {label} | {events[key].strftime('%H:%M:%S')} |")
    lines += [
        "",
        "## Bằng chứng",
        "",
        f"- Bản ghi trong cửa sổ kiểm tra: {analysis.total}",
        f"- Bản ghi lấy mẫu khi server tắt và được gửi bù sau khi bật lại: {analysis.backlog}",
        f"- Độ trễ giao nhận lớn nhất: {analysis.max_delivery_delay_s:.0f} s",
        f"- Điện thoại xem được dữ liệu khi server tắt: "
        f"{'có' if phone_ok else 'không' if phone_ok is False else 'không ghi nhận'}",
    ]
    if events.get("ap_status"):
        status = events["ap_status"]
        lines.append(f"- Trạng thái thiết bị qua Wi-Fi AP lúc offline: pending_records="
                     f"{status.get('pending_records')}, server_connected="
                     f"{status.get('server_connected')}, storage={status.get('storage')}")
    lines += ["", "## Tính liên tục của seq", "",
              "| boot_id | seq đầu | seq cuối | nhận | thiếu | trùng |",
              "|---|---|---|---|---|---|"]
    for boot in analysis.boots:
        lines.append(f"| `{boot.boot_id}` | {boot.first_seq} | {boot.last_seq} | {boot.received} | "
                     f"{compress(boot.missing)} | {boot.duplicates} |")
    lines += ["", "Ghi chú: `seq` tăng 1 mỗi lần lấy mẫu trong một `boot_id`. Bản trùng bị server "
              "bỏ qua nên luôn là 0 khi đọc qua API; bản thiếu nghĩa là dữ liệu bị mất.", ""]
    lines += [f"- {note}" for note in notes]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--api", default=os.environ.get("SAEMS_API_URL", DEFAULT_API))
    parser.add_argument("--device", default="farm-01")
    parser.add_argument("--offline-minutes", type=float, default=5.0)
    parser.add_argument("--sync-timeout-minutes", type=float, default=10.0)
    parser.add_argument("--ssh", help="user@pi to stop/start saems-mqtt automatically")
    parser.add_argument("--analyze-only", action="store_true")
    parser.add_argument("--from", dest="start")
    parser.add_argument("--to", dest="end")
    parser.add_argument("--report", type=Path,
                        default=Path(f"stage4-report-{datetime.now():%Y%m%d-%H%M}.md"))
    args = parser.parse_args()
    api = Api(args.api, login(args.api))
    notes: list[str] = []

    if args.analyze_only:
        start, end = parse_time(args.start), parse_time(args.end)
        if not start or not end:
            parser.error("--analyze-only needs --from and --to")
        analysis = analyze(api.telemetry(args.device, start, end))
        passed = report(args.report, args.device, analysis, {"start": start}, notes)
        print(f"Report: {args.report} -> {'PASS' if passed else 'FAIL'}")
        return 0 if passed else 1

    events: dict = {"start": datetime.now(timezone.utc)}
    print("1/5 Kiểm tra thiết bị đang gửi dữ liệu...")
    latest = api.get(f"/api/v1/devices/{args.device}/telemetry/latest")
    print(f"    seq={latest.get('seq')} boot={latest.get('boot_id')} lúc {latest.get('received_at')}")
    time.sleep(2)

    print("2/5 Tắt server")
    broker("stop", args.ssh)
    events["stopped"] = datetime.now(timezone.utc)
    wait(args.offline_minutes / 2, "    Thiết bị lưu offline")

    print("3/5 Kết nối điện thoại trực tiếp (BLE hoặc Wi-Fi AP của ESP32) và mở Tổng quan")
    events["phone"] = datetime.now(timezone.utc)
    events["ap_status"] = ap_status()
    if events["ap_status"] is None:
        notes.append("Không đọc được /v1/status qua AP (laptop không nối AP hoặc thiếu "
                     "SAEMS_DEVICE_TOKEN); bằng chứng điện thoại dựa trên xác nhận thủ công.")
    events["phone_ok"] = yes("    Điện thoại có hiện dữ liệu mới (seq tăng) không?")
    wait(args.offline_minutes / 2, "    Tiếp tục offline")

    print("4/5 Bật lại server")
    broker("start", args.ssh)
    events["restarted"] = datetime.now(timezone.utc)

    print("5/5 Chờ thiết bị gửi bù...")
    deadline = time.monotonic() + args.sync_timeout_minutes * 60
    events["synced"] = False
    analysis = None
    while time.monotonic() < deadline:
        time.sleep(15)
        try:
            records = api.telemetry(args.device, events["start"] - timedelta(minutes=1),
                                    datetime.now(timezone.utc) + timedelta(minutes=1))
        except (urllib.error.URLError, OSError) as error:
            print(f"    API chưa sẵn sàng: {error}")
            continue
        analysis = analyze(records, events["stopped"], events["restarted"])
        newest = max((parse_time(item.get("received_at")) for item in records
                      if item.get("received_at")), default=None)
        print(f"    {analysis.total} bản ghi, gửi bù {analysis.backlog}, thiếu {analysis.missing_total}")
        if analysis.backlog > 0 and analysis.missing_total == 0 and newest and \
                newest > events["restarted"]:
            events["synced"] = True
            events["synced_at"] = datetime.now(timezone.utc)
            break
    if analysis is None:
        print("Không lấy được dữ liệu từ API.")
        return 2
    if not events["synced"]:
        notes.append(f"Chưa đồng bộ đủ sau {args.sync_timeout_minutes:g} phút.")
    passed = report(args.report, args.device, analysis, events, notes)
    print(f"Report: {args.report} -> {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
