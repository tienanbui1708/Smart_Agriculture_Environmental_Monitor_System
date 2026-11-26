#!/usr/bin/env bash
# Build, flash, read or monitor the SAEMS ESP32-S3 firmware on Linux.
#
#   scripts/linux/run_device.sh build   [--profile direct|light|battery]
#   scripts/linux/run_device.sh flash   [-p /dev/ttyACM0] [--full-backup]
#   scripts/linux/run_device.sh read    [-p /dev/ttyACM0] [--seconds 30]
#   scripts/linux/run_device.sh monitor [-p /dev/ttyACM0]
#
# Linux counterpart of scripts/windows/run_device.ps1. Flash keeps NVS (Wi-Fi,
# soil calibration, power mode) and the offline telemetry log; on a board with
# the old partition table it erases only the NVS expansion 0xF000..0x39000.
# ESP-IDF location: $IDF_PATH, else ~/esp/esp-idf-v6.0.3.
set -euo pipefail

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

action="${1:-}"
[[ -z "$action" || "$action" == -h || "$action" == --help ]] && usage
shift
port="${SAEMS_PORT:-/dev/ttyACM0}"
profile="direct"
soil_raw="--soil-raw"
full_backup=0
seconds=30
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port) port="$2"; shift 2 ;;
        --profile) profile="$2"; shift 2 ;;
        --no-soil-raw) soil_raw=""; shift ;;
        --full-backup) full_backup=1; shift ;;
        --seconds) seconds="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; usage 1 ;;
    esac
done
case "$profile" in direct|light|battery) ;; *) echo "--profile must be direct, light or battery" >&2; exit 1 ;; esac

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
firmware="$repo/firmware/esp32s3"
build="$firmware/build-internet"
local_dir="$repo/.local"
win_scripts="$repo/scripts/windows"

die() { echo "[SAEMS device] $*" >&2; exit 1; }

load_idf() {
    local idf="${IDF_PATH:-$HOME/esp/esp-idf-v6.0.3}"
    [[ -f "$idf/export.sh" ]] || die "ESP-IDF not found at $idf. Install v6.0.3:
  git clone --depth 1 -b v6.0.3 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.3
  ~/esp/esp-idf-v6.0.3/install.sh esp32s3"
    # shellcheck disable=SC1091
    . "$idf/export.sh" >/dev/null 2>&1 || die "Could not activate ESP-IDF at $idf"
}

# idf.py prints Kconfig defaults; hide any configured password/token values.
redact() {
    python3 -I -c '
import json, pathlib, re, sys
repo = pathlib.Path(sys.argv[1]); secrets = set()
cfg = repo / "firmware/esp32s3/sdkconfig"
if cfg.exists():
    for line in cfg.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if sep and ("PASSWORD" in key or "TOKEN" in key):
            try:
                v = json.loads(value)
                if isinstance(v, str) and v: secrets.add(v)
            except json.JSONDecodeError: pass
for name in ("device-access.json", "firmware-ap-access.json", "wifi.json"):
    p = repo / ".local" / name
    if p.exists():
        for k, v in json.loads(p.read_text(encoding="utf-8-sig")).items():
            if v and re.search(r"password|token", k, re.I): secrets.add(str(v))
ordered = sorted(secrets, key=len, reverse=True)
for line in sys.stdin:
    for s in ordered: line = line.replace(s, "[REDACTED]")
    sys.stdout.write(line); sys.stdout.flush()
' "$repo"
}

idf() { (cd "$firmware" && idf.py "$@" 2>&1) | redact; }

# Drop noisy lines without turning "nothing left to print" into a failure.
quiet() { grep -vE "$1" || true; }

check_port() {
    [[ -e "$port" ]] || die "$port not found. Plug in the ESP32-S3 USB (UART) port or pass -p /dev/ttyUSB0."
    [[ -r "$port" && -w "$port" ]] || die "No permission on $port. Run once: sudo usermod -aG \$(stat -c %G $port) \$USER (then log in again),
or for this session only: sudo chmod 666 $port"
    if command -v fuser >/dev/null && fuser "$port" >/dev/null 2>&1; then
        die "$port is in use by another program (serial monitor?). Close it and retry."
    fi
}

reset_stale_build() {
    local cache="$build/CMakeCache.txt" cached
    [[ -f "$cache" ]] || return 0
    cached="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" | head -1)"
    if [[ -n "$cached" && "$(realpath -m "$cached")" != "$(realpath "$firmware")" ]]; then
        echo "Build cache was generated for $cached; removing build-internet."
        rm -rf "$build"
    fi
}

build_firmware() {
    reset_stale_build
    if [[ ! -f "$firmware/sdkconfig" ]]; then
        if ! idf -B build-internet set-target esp32s3 >/dev/null; then
            die "idf.py set-target failed"
        fi
    fi
    # shellcheck disable=SC2086
    python3 -I "$repo/scripts/linux/prepare_firmware_config.py" --power-profile "$profile" $soil_raw \
        || die "Could not prepare sdkconfig."
    if ! idf -B build-internet build | quiet "Deprecation notice"; then
        die "ESP-IDF build failed."
    fi
    local bin="$build/saems_esp32s3.bin"
    [[ -f "$bin" ]] || die "saems_esp32s3.bin was not produced."
    echo "BUILD OK: firmware/esp32s3/build-internet/saems_esp32s3.bin"
    echo "SHA256: $(sha256sum "$bin" | cut -d' ' -f1)"
}

esptool_read() { # address size output
    python -m esptool --chip esp32s3 --port "$port" --baud "$4" --before default-reset \
        --after no-reset --connect-attempts 5 read-flash "$1" "$2" "$3"
}

flash_firmware() {
    check_port
    build_firmware
    mkdir -p "$local_dir"
    local snapshot="esp32-lm393-current-nvs.bin" migration_args
    rm -f "$local_dir/$snapshot"
    if [[ $full_backup -eq 1 ]]; then
        local backup="$local_dir/esp32-before-lm393-current.bin"
        if [[ -f "$backup" ]]; then
            [[ $(stat -c %s "$backup") -eq 16777216 ]] || die "Backup exists but is not 16 MB: $backup"
            echo "Using existing 16 MB flash backup; will not overwrite it."
        else
            echo "Backing up all 16 MB flash (a few minutes)..."
            esptool_read 0 0x1000000 "$backup.partial" 921600 || { rm -f "$backup.partial"; die "Flash backup failed; nothing was erased."; }
            mv "$backup.partial" "$backup"
            echo "BACKUP OK: $backup"
        fi
        migration_args=(--backup "$(basename "$backup")" --snapshot "$snapshot")
    else
        local probe="$local_dir/esp32-migration-probe.bin"
        rm -f "$probe"
        echo "Reading 28 KiB partition/NVS probe..."
        esptool_read 0x8000 0x7000 "$probe" 115200 >/dev/null \
            || { rm -f "$probe"; die "Partition/NVS probe failed; stopped before erase or flash."; }
        migration_args=(--backup "$(basename "$probe")" --snapshot "$snapshot" --base-address 0x8000)
    fi
    local out
    out="$(python3 -I "$win_scripts/check_flash_migration.py" "${migration_args[@]}" 2>&1)" \
        || { echo "$out"; die "Partition layout is unexpected; stopped before flash."; }
    echo "$out"
    if grep -q 'MIGRATION_REQUIRED=1' <<<"$out"; then
        echo "Old partition: erasing only NVS expansion 0xF000..0x39000."
        python -m esptool --chip esp32s3 --port "$port" erase-region 0xF000 0x2A000 \
            || die "NVS expansion erase failed; stopped before writing firmware."
    else
        echo "SAEMS 192 KiB NVS partition detected; no expansion erase needed."
    fi
    if ! idf -B build-internet -p "$port" flash | quiet "Writing at|Deprecation notice"; then
        die "Flash failed."
    fi
    echo "FLASH OK: SAEMS firmware written to ESP32-S3."
    echo "Next: scripts/linux/run_device.sh read -p $port"
}

load_idf
case "$action" in
    build) build_firmware ;;
    flash) flash_firmware ;;
    read)
        check_port
        python "$win_scripts/read_device_log.py" --port "$port" --seconds "$seconds" --output device-read.log
        ;;
    monitor)
        check_port
        (cd "$firmware" && exec idf.py -B build-internet -p "$port" monitor)
        ;;
    *) echo "Unknown action: $action" >&2; usage 1 ;;
esac
