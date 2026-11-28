# Shared helpers for the SAEMS Linux scripts. Source it, do not run it.
# shellcheck shell=bash

SAEMS_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SAEMS_LOCAL="$SAEMS_REPO/.local"
SAEMS_API="${SAEMS_API_URL:-https://admin.tailb1f0ba.ts.net}"
SAEMS_PI="${SAEMS_PI:-admin@100.104.200.110}"
SAEMS_DEVICE="${SAEMS_DEVICE_ID:-farm-01}"
SAEMS_AP_BASE="http://192.168.4.1"

die() { echo "[SAEMS] $*" >&2; exit 1; }

# Print one key of a private JSON file in .local without echoing anything else.
local_json() { # file key
    local path="$SAEMS_LOCAL/$1"
    [[ -f "$path" ]] || die "Missing .local/$1"
    python3 -I -c 'import json,sys; v=json.load(open(sys.argv[1],encoding="utf-8-sig")).get(sys.argv[2]); print(v if v is not None else "")' \
        "$path" "$2"
}

# Serial of the first physical Android phone (not an emulator), or $SAEMS_PHONE.
phone_serial() {
    command -v adb >/dev/null || die "adb not found (Arch: pacman -S android-tools)."
    if [[ -n "${SAEMS_PHONE:-}" ]]; then echo "$SAEMS_PHONE"; return; fi
    local serial
    serial="$(adb devices | awk 'NR>1 && $2=="device" && $1 !~ /^emulator-/ {print $1; exit}')"
    if [[ -z "$serial" ]]; then
        if adb devices | awk 'NR>1' | grep -q unauthorized; then
            die "Phone is connected but not authorized: accept 'Allow USB debugging' on the phone."
        fi
        die "No Android phone over ADB. Enable USB debugging and replug the cable."
    fi
    echo "$serial"
}

# Bearer token for the device REST API on the Wi-Fi AP.
device_token() {
    if [[ -n "${SAEMS_DEVICE_TOKEN:-}" ]]; then echo "$SAEMS_DEVICE_TOKEN"; return; fi
    if [[ -f "$SAEMS_LOCAL/firmware-ap-access.json" ]]; then local_json firmware-ap-access.json password; return; fi
    local token
    read -r -s -p "Device token (AP password unless changed): " token; echo >&2
    echo "$token"
}

require_ap_link() {
    ip -4 -o addr show | grep -q ' 192\.168\.4\.' \
        || die "This laptop is not on the ESP32 AP. Join Wi-Fi SAEMS-farm-01 first (nmcli dev wifi connect SAEMS-farm-01 --ask)."
}
