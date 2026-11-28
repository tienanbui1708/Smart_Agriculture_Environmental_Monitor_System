"""Write Wi-Fi, MQTT and power profile into the ignored firmware sdkconfig (Linux).

Linux counterpart of scripts/windows/prepare_firmware_acceptance.py. Secrets come
from the ignored .local directory and are never printed:

  .local/wifi.json            {"ssid": "...", "password": "..."}  (prompted once if missing)
  .local/device-access.json   MQTT login exported by the mini-server
                              (copy from the Pi: saems-mini-server/device-access.json)
  .local/firmware-ap-access.json  Wi-Fi AP password (generated if missing)
"""
import argparse
import getpass
import json
import os
import secrets
import shutil
import sys
from pathlib import Path

DEFAULT_BROKER = "mqtts://admin.tailb1f0ba.ts.net:8443"

parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--power-profile", choices=("direct", "light", "battery"),
                    default="direct",
                    help="First-boot default when NVS has no power_mode")
parser.add_argument("--soil-raw", action="store_true",
                    help="Log raw LM393 ADC values")
parser.add_argument("--broker", default=os.environ.get("SAEMS_MQTT_BROKER_URI", DEFAULT_BROKER),
                    help="MQTT broker URI (default: %(default)s)")
args = parser.parse_args()

repo = Path(__file__).resolve().parents[2]
private = repo / ".local"
private.mkdir(mode=0o700, exist_ok=True)
config_path = repo / "firmware/esp32s3/sdkconfig"
if not config_path.exists():
    sys.exit("firmware/esp32s3/sdkconfig is missing; run 'idf.py set-target esp32s3' first")


def write_private(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    path.chmod(0o600)


wifi_path = private / "wifi.json"
if wifi_path.exists():
    wifi = json.loads(wifi_path.read_text(encoding="utf-8"))
elif sys.stdin.isatty():
    print(f"{wifi_path.relative_to(repo)} not found; enter the Wi-Fi network for the device.")
    wifi = {"ssid": input("Wi-Fi SSID: ").strip(),
            "password": getpass.getpass("Wi-Fi password (WPA2): ")}
    write_private(wifi_path, wifi)
else:
    sys.exit(f"Missing {wifi_path}; create it with keys 'ssid' and 'password'")
if not wifi.get("ssid"):
    sys.exit("Wi-Fi SSID is empty in .local/wifi.json")

access_path = private / "device-access.json"
if not access_path.exists():
    sys.exit("Missing .local/device-access.json. Copy it from the Pi, e.g.\n"
             "  scp admin@<pi>:saems-mini-server/device-access.json .local/")
access = json.loads(access_path.read_text(encoding="utf-8-sig"))

ap_path = private / "firmware-ap-access.json"
if ap_path.exists():
    ap_access = json.loads(ap_path.read_text(encoding="utf-8"))
else:
    ap_access = {"ssid": "SAEMS-farm-01", "password": secrets.token_urlsafe(18)}
    write_private(ap_path, ap_access)

backup = private / "sdkconfig-before-hardware-test"
if not backup.exists():
    shutil.copy2(config_path, backup)
    backup.chmod(0o600)

profile_settings = {
    "direct": {"CONFIG_SAEMS_POWER_MODE_MODEM_SAVE": "y",
               "CONFIG_SAEMS_POWER_MODE_LIGHT_SLEEP": "n",
               "CONFIG_SAEMS_POWER_MODE_DEEP_SLEEP": "n"},
    "light": {"CONFIG_SAEMS_POWER_MODE_MODEM_SAVE": "n",
              "CONFIG_SAEMS_POWER_MODE_LIGHT_SLEEP": "y",
              "CONFIG_SAEMS_POWER_MODE_DEEP_SLEEP": "n"},
    "battery": {"CONFIG_SAEMS_POWER_MODE_MODEM_SAVE": "n",
                "CONFIG_SAEMS_POWER_MODE_LIGHT_SLEEP": "n",
                "CONFIG_SAEMS_POWER_MODE_DEEP_SLEEP": "y"},
}
settings = {
    "CONFIG_SAEMS_WIFI_SSID": json.dumps(wifi["ssid"]),
    "CONFIG_SAEMS_WIFI_PASSWORD": json.dumps(wifi.get("password", "")),
    "CONFIG_SAEMS_MQTT_BROKER_URI": json.dumps(args.broker),
    "CONFIG_SAEMS_MQTT_USERNAME": json.dumps(access["mqtt_username"]),
    "CONFIG_SAEMS_MQTT_PASSWORD": json.dumps(access["mqtt_password"]),
    "CONFIG_SAEMS_DEVICE_ID": json.dumps(access["device_id"]),
    "CONFIG_SAEMS_AP_PASSWORD": json.dumps(ap_access["password"]),
    "CONFIG_SAEMS_POWER_SAVE_ENABLED": "y",
    "CONFIG_SAEMS_DEEP_SLEEP_SYNC_WINDOW_SECONDS": "15",
    "CONFIG_SAEMS_BATTERY_MAINTENANCE_WINDOW_SECONDS": "10",
    "CONFIG_SAEMS_SOIL_SENSOR_ENABLED": "y",
    "CONFIG_SAEMS_SOIL_RAW_READOUT_ENABLED": "y" if args.soil_raw else "n",
    "CONFIG_SAEMS_SOIL_DRY_RAW": "4095",
    "CONFIG_SAEMS_SOIL_WET_RAW": "2080",
    "CONFIG_SAEMS_SOIL_DEFAULTS_MEASURED": "y",
    "CONFIG_MBEDTLS_HAVE_TIME_DATE": "y",
    "CONFIG_LWIP_SNTP_MAX_SERVERS": "3",
    "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE": "8192",
    "CONFIG_BT_NIMBLE_TASK_STACK_SIZE": "8192",
    "CONFIG_SAEMS_POWER_MODE_ACTIVE": "n",
    "CONFIG_PM_ENABLE": "y",
    "CONFIG_FREERTOS_USE_TICKLESS_IDLE": "y",
    "CONFIG_SAEMS_BLE_ENABLED": "y",
    "CONFIG_SAEMS_AP_ENABLED": "y",
}
settings.update(profile_settings[args.power_profile])

lines = []
seen = set()
for line in config_path.read_text(encoding="utf-8").splitlines():
    key = line.split("=", 1)[0]
    if line.startswith("# CONFIG_") and line.endswith(" is not set"):
        key = line[2:].removesuffix(" is not set")
    if key in settings:
        value = settings[key]
        line = f"# {key} is not set" if value == "n" else f"{key}={value}"
        seen.add(key)
    lines.append(line)
for key in sorted(settings.keys() - seen):
    value = settings[key]
    lines.append(f"# {key} is not set" if value == "n" else f"{key}={value}")
config_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
config_path.chmod(0o600)
print(f"Prepared ignored sdkconfig: Wi-Fi + MQTT ({args.broker}), default power mode="
      f"{args.power_profile}, soil raw log={'on' if args.soil_raw else 'off'}; "
      "credential values omitted")
