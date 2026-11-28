#!/usr/bin/env bash
# Talk to the ESP32 REST API over its Wi-Fi AP (laptop must join SAEMS-farm-01).
#
#   scripts/linux/device_ap.sh status
#   scripts/linux/device_ap.sh power                      # read power mode
#   scripts/linux/device_ap.sh power direct|light|battery [interval_s]
#   scripts/linux/device_ap.sh soil                       # read soil calibration + reading
#   scripts/linux/device_ap.sh soil-calibrate <dry_raw> <wet_raw>
#
# Token: $SAEMS_DEVICE_TOKEN, else the AP password in .local/firmware-ap-access.json.
# Linux counterpart of scripts/windows/power_mode.ps1 and soil_calibration.ps1.
set -euo pipefail
# shellcheck source=common.sh
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

usage() { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
action="${1:-}"; [[ -z "$action" || "$action" == -h || "$action" == --help ]] && usage
shift
require_ap_link
token="$(device_token)"

api() { # method path [json]
    local args=(-sS --fail-with-body --max-time 10 -H "Authorization: Bearer $token" -X "$1")
    [[ $# -ge 3 ]] && args+=(-H "Content-Type: application/json" --data "$3")
    curl "${args[@]}" "$SAEMS_AP_BASE$2" || die "Request $1 $2 failed (Battery mode: retry while the device is awake)."
}
pick() { # keys... (reads JSON on stdin)
    python3 -I -c 'import json,sys; d=json.load(sys.stdin); [print(f"{k:26} {d.get(k)}") for k in sys.argv[1:]]' "$@"
}
check_device() {
    local id; id="$(api GET /v1/config | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("device_id"))')"
    [[ "$id" == "$SAEMS_DEVICE" ]] || die "Unexpected device '$id' (expected $SAEMS_DEVICE); refusing."
}

case "$action" in
    status) api GET /v1/status | python3 -I -m json.tool ;;
    power)
        check_device
        if [[ $# -ge 1 ]]; then
            mode="$1"; [[ "$mode" =~ ^(direct|light|battery)$ ]] || die "Mode must be direct, light or battery."
            body="{\"power_mode\":\"$mode\""
            if [[ $# -ge 2 ]]; then
                [[ "$2" =~ ^[0-9]+$ && "$2" -ge 1 && "$2" -le 3600 ]] || die "Interval must be 1..3600 s."
                body+=",\"telemetry_interval_s\":$2"
            fi
            api PUT /v1/config "$body}" >/dev/null
            got="$(api GET /v1/config | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("power_mode"))')"
            [[ "$got" == "$mode" ]] || die "Readback is '$got', not '$mode'."
            echo "Power mode '$mode' saved; it applies at the next sample, no reflash needed."
        fi
        api GET /v1/config | pick device_id power_mode telemetry_interval_s config_version ap_enabled ble_enabled ;;
    soil)
        check_device
        api GET /v1/config | pick soil_enabled soil_calibrated soil_raw_readout_enabled soil_dry_raw soil_wet_raw
        api GET /v1/telemetry/latest | pick boot_id seq soil_raw soil_digital soil_moisture_pct sensor_status ;;
    soil-calibrate)
        [[ $# -eq 2 && "$1" =~ ^[0-9]+$ && "$2" =~ ^[0-9]+$ && "$1" -le 4095 && "$2" -le 4095 ]] \
            || die "Give both measured values: soil-calibrate <dry_raw> <wet_raw> (0..4095)."
        span=$(( $1 > $2 ? $1 - $2 : $2 - $1 ))
        [[ $span -ge 100 ]] || die "ADC span below 100 counts: check wiring and measure again."
        check_device
        api PUT /v1/config "{\"soil_enabled\":true,\"soil_calibrated\":true,\"soil_dry_raw\":$1,\"soil_wet_raw\":$2}" >/dev/null
        api GET /v1/config | python3 -I -c '
import json, sys
d = json.load(sys.stdin); dry, wet = int(sys.argv[1]), int(sys.argv[2])
ok = d.get("soil_calibrated") and d.get("soil_enabled") and d.get("soil_dry_raw") == dry and d.get("soil_wet_raw") == wet
print("Calibration saved and verified; press EN/reset on the ESP32 to apply." if ok else "Readback mismatch: do not trust the percentage.")
sys.exit(0 if ok else 1)' "$1" "$2" ;;
    *) echo "Unknown action: $action" >&2; usage 1 ;;
esac
