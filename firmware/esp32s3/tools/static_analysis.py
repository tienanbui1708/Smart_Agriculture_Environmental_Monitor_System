#!/usr/bin/env python3
"""Static analysis for the SAEMS firmware: cppcheck (+ MISRA C:2012 addon)
and clang-tidy. Writes a Markdown report.

Run from firmware/esp32s3:

    python tools/static_analysis.py --build-dir <idf build dir> \
        --report ../../docs/static-analysis-report.md

Gates (non-zero exit when violated):
  1. MISRA C:2012 on the pure-C modules (STRICT_SOURCES): no finding except
     the deviations listed in tools/misra_suppressions.txt.
  2. cppcheck warning/error findings on every firmware source.
  3. clang-tidy findings (bugprone, cert, clang-analyzer) on firmware
     sources, when --build-dir provides compile_commands.json.
MISRA findings in ESP-IDF integration code are reported per rule but do not
fail the run (see docs/misra-deviations.md).

Tools: cppcheck >= 2.14 with addons/misra.py; clang-tidy from esp-clang
(understands Xtensa). Paths are discovered or given with --cppcheck /
--clang-tidy.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import glob
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STRICT_SOURCES = [
    "components/threshold_monitor/threshold_monitor.c",
    "components/irrigation/irrigation.c",
    "components/battery_monitor/battery_policy.c",
    "components/record_log/record_log.c",
    "components/status_led/status_led_policy.c",
]
SUPPRESSIONS = ROOT / "tools" / "misra_suppressions.txt"
FINDING = re.compile(r"^(?P<file>[^:]+(?::[\\/][^:]+)?):(?P<line>\d+):(?P<severity>\w+):"
                     r"(?P<id>[\w.-]+):(?P<message>.*)$")
TIDY_CHECKS = ",".join([
    "-*",
    "bugprone-*",
    "cert-*",
    "clang-analyzer-*",
    # Style-level checks that conflict with the ESP-IDF API shape.
    "-bugprone-easily-swappable-parameters",
    "-bugprone-assignment-in-if-condition",
    "-cert-dcl37-c",
    "-cert-dcl51-cpp",
    # Suggests C11 Annex K (memcpy_s, snprintf_s); picolibc has no Annex K,
    # and every call here is bounded by an explicit size.
    "-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling",
])
GCC_ONLY_FLAGS = (
    "-mlongcalls", "-fno-shrink-wrap", "-fstrict-volatile-bitfields",
    "-fno-tree-switch-conversion", "-fzero-init-padding-bits=all",
    "-fno-malloc-dce", "-Wno-old-style-declaration", "-fno-jump-tables",
    "-fdiagnostics-color=always", "-Werror",
)


def project_sources() -> list[str]:
    files = sorted(glob.glob("components/*/*.c", root_dir=ROOT) +
                   glob.glob("main/*.c", root_dir=ROOT))
    return [path.replace("\\", "/") for path in files]


def include_dirs() -> list[str]:
    return sorted({str(Path(path).parent).replace("\\", "/")
                   for path in glob.glob("components/*/include/*.h", root_dir=ROOT)})


def find_tool(explicit: str | None, names: list[str], globs: list[str]) -> str | None:
    if explicit:
        return explicit
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    for pattern in globs:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[-1]
    return None


def misra_addon(cppcheck: str) -> str:
    base = Path(cppcheck).resolve().parent
    for candidate in (base / "addons" / "misra.py",
                      base.parent / "share" / "Cppcheck" / "addons" / "misra.py",
                      base.parent / "share" / "cppcheck" / "addons" / "misra.py"):
        if candidate.exists():
            return str(candidate)
    raise SystemExit("cppcheck MISRA addon (misra.py) not found")


def parse(output: str) -> list[dict]:
    findings = []
    for line in output.splitlines():
        match = FINDING.match(line.strip())
        if match and match["severity"] != "information":
            findings.append(match.groupdict())
    return findings


def run_cppcheck(cppcheck: str, sources: list[str], misra: bool,
                 extra: list[str]) -> list[dict]:
    command = [cppcheck, "--std=c11", "--quiet", "--inline-suppr",
               "--enable=warning,style,performance,portability",
               "--template={file}:{line}:{severity}:{id}:{message}",
               "--suppress=missingInclude", "--suppress=missingIncludeSystem",
               "--suppress=unmatchedSuppression", *extra]
    command += [f"-I{path}" for path in include_dirs()]
    if misra:
        command += [f"--addon={misra_addon(cppcheck)}", f"--addon-python={sys.executable}"]
    result = subprocess.run(command + sources, cwd=ROOT, capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    return parse(result.stderr + result.stdout)


def sanitized_compile_db(build_dir: Path, sources: list[str], work: Path) -> Path:
    """Rewrite the GCC compile database for clang-tidy (esp-clang)."""
    database = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    wanted = {str((ROOT / path).resolve()).lower() for path in sources}
    toolchain = sorted(glob.glob(str(Path(os.environ.get("IDF_TOOLS_PATH", "C:/Espressif/tools"))
                                     / "xtensa-esp-elf" / "*" / "xtensa-esp-elf")))
    picolibc = [f"-isystem{toolchain[-1]}/picolibc/include"] if toolchain else []
    entries = []
    for entry in database:
        if str(Path(entry["file"]).resolve()).lower() not in wanted:
            continue
        command = entry.get("command") or " ".join(entry["arguments"])
        cflags = build_dir / "toolchain" / "cflags"
        command = command.replace(f'@"{cflags.as_posix()}"', "").replace(f"@{cflags.as_posix()}", "")
        tokens = [token for token in command.split(" ")
                  if token and not token.startswith(GCC_ONLY_FLAGS) and
                  not token.startswith("-specs=") and "picolibc.specs" not in token]
        tokens[0] = "clang"
        tokens[1:1] = ["--target=xtensa-esp-elf", "-mcpu=esp32s3",
                       "-D__XTENSA__", *picolibc]
        entries.append({"directory": entry["directory"], "file": entry["file"],
                        "command": " ".join(tokens)})
    (work / "compile_commands.json").write_text(json.dumps(entries, indent=1), encoding="utf-8")
    return work


def run_clang_tidy(clang_tidy: str, build_dir: Path, sources: list[str]) -> list[dict]:
    findings = []
    with tempfile.TemporaryDirectory() as folder:
        database = sanitized_compile_db(build_dir, sources, Path(folder))
        for source in sources:
            result = subprocess.run(
                [clang_tidy, "-p", str(database), f"--checks={TIDY_CHECKS}",
                 "--quiet", "--extra-arg=-Wno-unknown-warning-option",
                 "--header-filter=.*[/\\\\]components[/\\\\].*", str(ROOT / source)],
                cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
            for line in result.stdout.splitlines():
                match = re.match(r"^(?P<file>.+?):(?P<line>\d+):\d+: (?P<severity>warning|error): "
                                 r"(?P<message>.*) \[(?P<id>[^\]]+)\]$", line)
                if not match or "clang-diagnostic" in match["id"]:
                    continue
                path = Path(match["file"]).resolve()
                if ROOT not in path.parents:
                    continue
                finding = match.groupdict()
                finding["file"] = path.relative_to(ROOT).as_posix()
                if finding not in findings:
                    findings.append(finding)
    return findings


def relative(path: str) -> str:
    try:
        return Path(path).resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return path.replace("\\", "/")


def table(findings: list[dict]) -> list[str]:
    if not findings:
        return ["Không có.", ""]
    lines = ["| File | Dòng | Mã | Nội dung |", "|---|---|---|---|"]
    for item in findings:
        message = item["message"].replace("|", "\\|")
        lines.append(f"| `{relative(item['file'])}` | {item['line']} | `{item['id']}` | {message} |")
    return lines + [""]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, help="ESP-IDF build dir (compile_commands.json, config/)")
    parser.add_argument("--cppcheck")
    parser.add_argument("--clang-tidy")
    parser.add_argument("--report", type=Path, help="Markdown report to write")
    args = parser.parse_args()

    cppcheck = find_tool(args.cppcheck, ["cppcheck"], [
        "C:/Program Files/Cppcheck/cppcheck.exe", "C:/mingw-w64/mingw64/bin/cppcheck.exe"])
    if not cppcheck:
        raise SystemExit("cppcheck not found (install it or pass --cppcheck)")
    clang_tidy = find_tool(args.clang_tidy, ["clang-tidy"], [
        "C:/Espressif/tools/esp-clang/*/esp-clang/bin/clang-tidy.exe",
        os.path.expanduser("~/.espressif/tools/esp-clang/*/esp-clang/bin/clang-tidy")])
    version = subprocess.run([cppcheck, "--version"], capture_output=True, text=True).stdout.strip()
    config_include = [f"-I{(args.build_dir / 'config').as_posix()}"] if args.build_dir else []
    sources = project_sources()

    strict = run_cppcheck(cppcheck, STRICT_SOURCES, True,
                          [f"--suppressions-list={SUPPRESSIONS.as_posix()}"])
    general = [item for item in run_cppcheck(cppcheck, sources, False, config_include)
               if item["severity"] in ("error", "warning")]
    platform_sources = [path for path in sources if path not in STRICT_SOURCES]
    platform_misra = Counter(
        item["id"] for item in run_cppcheck(cppcheck, platform_sources, True, config_include)
        if item["id"].startswith("misra"))
    tidy: list[dict] = []
    tidy_note = "bỏ qua (thiếu --build-dir hoặc clang-tidy)"
    if args.build_dir and clang_tidy and (args.build_dir / "compile_commands.json").exists():
        tidy = run_clang_tidy(clang_tidy, args.build_dir, sources)
        tidy_version = subprocess.run([clang_tidy, "--version"], capture_output=True,
                                      text=True).stdout.split("\n")[1].strip()
        tidy_note = f"{tidy_version}, checks `{TIDY_CHECKS}`"

    deviations = [line.split(":")[0] for line in SUPPRESSIONS.read_text(encoding="utf-8").splitlines()
                  if line and not line.startswith("#")]
    report = [
        "# Báo cáo phân tích tĩnh firmware",
        "",
        f"Tạo lúc {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')} bằng "
        "`firmware/esp32s3/tools/static_analysis.py`. Deviation và lý do: "
        "[`misra-deviations.md`](misra-deviations.md).",
        "",
        f"- cppcheck: {version}, addon MISRA C:2012",
        f"- clang-tidy: {tidy_note}",
        f"- Nguồn: {len(sources)} file C của project (không tính ESP-IDF, managed_components)",
        "",
        "| Cổng | Phạm vi | Kết quả |",
        "|---|---|---|",
        f"| 1. MISRA C:2012 nghiêm ngặt | {len(STRICT_SOURCES)} module C thuần | "
        f"{'ĐẠT' if not strict else 'KHÔNG ĐẠT'} ({len(strict)} vi phạm; deviation: "
        f"{', '.join(sorted(set(deviations)))}) |",
        f"| 2. cppcheck lỗi/cảnh báo | toàn bộ firmware | "
        f"{'ĐẠT' if not general else 'KHÔNG ĐẠT'} ({len(general)}) |",
        f"| 3. clang-tidy | toàn bộ firmware | "
        f"{'ĐẠT' if not tidy else 'KHÔNG ĐẠT'} ({len(tidy)}) |",
        "",
        "## 1. MISRA trên module C thuần",
        "",
        *table(strict),
        "## 2. cppcheck (warning/error)",
        "",
        *table(general),
        "## 3. clang-tidy",
        "",
        *table(tidy),
        "## 4. MISRA trên phần tích hợp ESP-IDF (tham khảo, không chặn)",
        "",
        "Phần này gọi API ESP-IDF/FreeRTOS/NimBLE (macro, con trỏ void, nhiều điểm return "
        "theo `ESP_RETURN_ON_ERROR`), nên chỉ thống kê để theo dõi xu hướng.",
        "",
        "| Quy tắc | Số lần |",
        "|---|---|",
        *[f"| `{rule}` | {count} |" for rule, count in platform_misra.most_common()],
        "",
    ]
    text = "\n".join(report)
    if args.report:
        args.report.write_text(text, encoding="utf-8")
        print(f"Report: {args.report}")
    print(f"MISRA strict: {len(strict)}  cppcheck: {len(general)}  clang-tidy: {len(tidy)}  "
          f"platform MISRA (info): {sum(platform_misra.values())}")
    for item in strict + general + tidy:
        print(f"  {relative(item['file'])}:{item['line']}: {item['id']}: {item['message']}")
    return 1 if strict or general or tidy else 0


if __name__ == "__main__":
    sys.exit(main())
