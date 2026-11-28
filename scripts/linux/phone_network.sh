#!/usr/bin/env bash
# Prepare the Android phone (ADB) for the direct-connection demos.
#
#   scripts/linux/phone_network.sh ap       # mobile data OFF, join ESP32 AP SAEMS-farm-01
#   scripts/linux/phone_network.sh ble      # Wi-Fi OFF, mobile data OFF (keep Bluetooth ON)
#   scripts/linux/phone_network.sh restore  # put Wi-Fi/mobile data back as they were
#   scripts/linux/phone_network.sh status
#
# Linux counterpart of scripts/windows/phone_ap_test.ps1 and phone_ble_test.ps1.
set -euo pipefail
# shellcheck source=common.sh
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

usage() { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
action="${1:-}"; [[ -z "$action" || "$action" == -h || "$action" == --help ]] && usage
serial="$(phone_serial)"
state="$SAEMS_LOCAL/phone-network-before-demo.txt"
sh() { adb -s "$serial" shell "$@" | tr -d '\r'; }

save_state() {
    mkdir -p "$SAEMS_LOCAL"
    if [[ -f "$state" ]]; then
        echo "Keeping the state saved before the first demo step (.local/$(basename "$state"))."
    else
        printf 'wifi=%s\ndata=%s\n' "$(sh settings get global wifi_on)" "$(sh settings get global mobile_data)" > "$state"
    fi
}

case "$action" in
    ap)
        save_state
        sh svc data disable; sh svc wifi enable; sleep 2
        sh cmd wifi connect-network "$(local_json firmware-ap-access.json ssid)" wpa2 \
            "$(local_json firmware-ap-access.json password)" >/dev/null || die "Could not request the ESP32 AP connection."
        sleep 5
        echo "AP DEMO READY: mobile data OFF, phone asked to join SAEMS-farm-01 (password omitted)."
        sh cmd wifi status | head -3
        echo "In the app: Kết nối trực tiếp -> Dùng Wi-Fi trực tiếp. Device token = AP password (.local/firmware-ap-access.json)." ;;
    ble)
        save_state
        sh svc wifi disable; sh svc data disable
        echo "BLE DEMO READY: Wi-Fi OFF, mobile data OFF. Keep Bluetooth ON."
        echo "In the app: Kết nối trực tiếp -> Quét ESP32 -> SAEMS-farm-01. Passkey is printed in the device log (run_device.sh monitor)." ;;
    restore)
        [[ -f "$state" ]] || die "No saved phone state; nothing to restore."
        # shellcheck disable=SC1090
        . "$state"
        sh cmd wifi disconnect >/dev/null 2>&1 || true
        if [[ "${wifi:-1}" == 1 ]]; then sh svc wifi enable; sh cmd wifi reconnect >/dev/null 2>&1 || true; else sh svc wifi disable; fi
        if [[ "${data:-1}" == 1 ]]; then sh svc data enable; else sh svc data disable; fi
        rm -f "$state"
        echo "PHONE NETWORK RESTORED (wifi=${wifi:-?} data=${data:-?})." ;;
    status)
        echo "wifi_on=$(sh settings get global wifi_on) mobile_data=$(sh settings get global mobile_data)"
        sh cmd wifi status | head -3 ;;
    *) echo "Unknown action: $action" >&2; usage 1 ;;
esac
