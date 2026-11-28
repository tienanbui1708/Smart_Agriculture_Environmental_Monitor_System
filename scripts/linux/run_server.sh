#!/usr/bin/env bash
# Check the SAEMS server on the Raspberry Pi and follow its data (Linux).
#
#   scripts/linux/run_server.sh check            # /health + latest telemetry
#   scripts/linux/run_server.sh latest
#   scripts/linux/run_server.sh watch [seconds]  # one line per new record (API <- SQLite)
#   scripts/linux/run_server.sh logs             # journalctl of broker, ingestor, API
#   scripts/linux/run_server.sh db [seconds]     # 5 newest SQLite rows, refreshed
#   scripts/linux/run_server.sh services         # systemd state on the Pi
#   scripts/linux/run_server.sh gaps             # seq continuity of the newest boot
#   scripts/linux/run_server.sh broker-stop|broker-start   # simulate server outage
#
# API token: .local/device-access.json (api_access_token) or $SAEMS_API_TOKEN.
# Pi over SSH: $SAEMS_PI (default admin@100.104.200.110 via Tailscale).
# Linux counterpart of scripts/windows/run_server.ps1.
set -euo pipefail
# shellcheck source=common.sh
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
action="${1:-check}"; [[ "$action" == -h || "$action" == --help ]] && usage
interval="${2:-2}"
db="/home/admin/saems-mini-server/data/telemetry.db"

api_token() {
    if [[ -n "${SAEMS_API_TOKEN:-}" ]]; then echo "$SAEMS_API_TOKEN"; else local_json device-access.json api_access_token; fi
}

latest_json() {
    curl -sSf --max-time 15 -H "Authorization: Bearer $(api_token)" \
        "$SAEMS_API/api/v1/devices/$SAEMS_DEVICE/telemetry/latest"
}

show_latest() {
    latest_json | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
for k in ("device_id","boot_id","seq","received_at","data_mode","temperature_c","humidity_pct",
          "soil_raw","soil_moisture_pct","light_lux","battery_pct","pump_state","alarm_mask","sensor_status"):
    print(f"{k:18} {d.get(k)}")'
}

pi() { ssh -o ConnectTimeout=10 "$SAEMS_PI" "$@"; }

case "$action" in
    check)
        echo "SERVER: $(curl -sSf --max-time 15 "$SAEMS_API/health")"
        show_latest ;;
    latest) show_latest ;;
    watch)
        echo "Watching $SAEMS_API every ${interval}s (Ctrl+C to stop)."
        prev=""
        while true; do
            if line="$(latest_json 2>/dev/null | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
g = lambda k: d.get(k)
soil = "--" if g("soil_moisture_pct") is None else g("soil_moisture_pct")
key = "%s/%s" % (g("boot_id"), g("seq"))
print("%s|%s | boot=%s seq=%s | T=%sC H=%s%% soil=%s%% lux=%s pump=%s alarm=%s" % (
    key, g("received_at"), g("boot_id"), g("seq"), g("temperature_c"), g("humidity_pct"),
    soil, g("light_lux"), g("pump_state"), g("alarm_mask")))')"; then
                key="${line%%|*}"
                [[ "$key" != "$prev" ]] && echo "${line#*|}"
                prev="$key"
            else
                echo "$(date -Is) API watch failed (server down or token invalid)" >&2
            fi
            sleep "$interval"
        done ;;
    logs) pi 'journalctl -u saems-mqtt -u saems-ingestor -u saems-legacy-ingestor -u saems-api -f -n 40 -o short-iso' ;;
    db) pi "while true; do date -Is; sqlite3 $db 'SELECT device_id,boot_id,seq,received_at FROM telemetry ORDER BY id DESC LIMIT 5;'; sleep $interval; done" ;;
    services) pi 'systemctl is-active saems-mqtt saems-ingestor saems-legacy-ingestor saems-api tailscaled | paste -sd" "; echo "(saems-mqtt saems-ingestor saems-legacy-ingestor saems-api tailscaled)"' ;;
    gaps)
        pi "sqlite3 -header -column $db \"SELECT boot_id, COUNT(*) AS rows, MIN(seq) AS first_seq, MAX(seq) AS last_seq,
            MAX(seq)-MIN(seq)+1-COUNT(DISTINCT seq) AS missing, MAX(received_at) AS last_rx
            FROM telemetry WHERE device_id='$SAEMS_DEVICE' GROUP BY boot_id ORDER BY last_rx DESC LIMIT 3;\""
        echo "missing = 0 means no record was lost for that boot." ;;
    broker-stop)
        pi 'sudo systemctl stop saems-mqtt && systemctl is-active saems-mqtt || true'
        echo "Broker stopped: the device should keep sampling offline (LED yellow, pending grows)." ;;
    broker-start)
        pi 'sudo systemctl start saems-mqtt && systemctl is-active saems-mqtt'
        echo "Broker started: the device should resend its backlog (LED purple, then green)." ;;
    *) echo "Unknown action: $action" >&2; usage 1 ;;
esac
