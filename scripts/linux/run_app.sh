#!/usr/bin/env bash
# Build, install or run the SAEMS Flutter app on an Android phone (Linux).
#
#   scripts/linux/run_app.sh build               # APK debug -> mobile/saems_app/build/app/outputs/flutter-apk/app-debug.apk
#   scripts/linux/run_app.sh install [--reinstall]
#   scripts/linux/run_app.sh run                 # flutter run with logs (q to quit)
#   scripts/linux/run_app.sh info                # installed version/date on the phone
#   scripts/linux/run_app.sh screenshot [name.png]
#
# Options: --mock (mock data), SAEMS_PHONE=<adb serial>, FLUTTER=<path to flutter>.
# An APK built here is signed with this machine's debug key. Over an app built
# on another computer, Android refuses the update; --reinstall uninstalls the
# old app first (its in-app settings and login are lost).
# Linux counterpart of scripts/windows/run_app.ps1.
set -euo pipefail
# shellcheck source=common.sh
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
action="${1:-}"; [[ -z "$action" || "$action" == -h || "$action" == --help ]] && usage
shift
mock=false; reinstall=0; shot="saems-screen.png"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mock) mock=true ;;
        --reinstall) reinstall=1 ;;
        *.png) shot="$1" ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; usage 1 ;;
    esac
    shift
done

app="$SAEMS_REPO/mobile/saems_app"
apk="$app/build/app/outputs/flutter-apk/app-debug.apk"
package="com.hirenge.saems.saems_app"
flutter="${FLUTTER:-$(command -v flutter || echo "$HOME/flutter/bin/flutter")}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
defines=(--dart-define=ENABLE_MOCK_MODE=$mock
         "--dart-define=API_BASE_URL=$SAEMS_API"
         "--dart-define=DEFAULT_DEVICE_ID=$SAEMS_DEVICE")

need_flutter() {
    [[ -x "$flutter" ]] || die "Flutter not found. Install 3.47.7 (Dart 3.13.5):
  curl -LO https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.47.7-stable.tar.xz
  tar -xJf flutter_linux_3.47.7-stable.tar.xz -C ~"
    [[ -d "$ANDROID_HOME" ]] || die "Android SDK not found at $ANDROID_HOME (set ANDROID_HOME)."
    (cd "$app" && "$flutter" pub get >/dev/null) || die "flutter pub get failed."
}

build() {
    need_flutter
    (cd "$app" && "$flutter" build apk --debug "${defines[@]}") || die "APK build failed."
    echo "APP BUILD OK: mobile/saems_app/build/app/outputs/flutter-apk/app-debug.apk"
}

info() {
    local serial; serial="$(phone_serial)"
    echo "Phone: $(adb -s "$serial" shell getprop ro.product.model | tr -d '\r') ($serial)"
    adb -s "$serial" shell dumpsys package "$package" | grep -E "versionName|lastUpdateTime|firstInstallTime" | sort -u \
        || echo "SAEMS app is not installed."
    echo "Last app code change: $(git -C "$SAEMS_REPO" log -1 --format='%ad %h %s' --date=format:'%Y-%m-%d %H:%M:%S' -- mobile/saems_app/lib mobile/saems_app/android mobile/saems_app/pubspec.yaml)"
}

case "$action" in
    build) build ;;
    install)
        serial="$(phone_serial)"
        build
        if ! out="$(adb -s "$serial" install -r "$apk" 2>&1)"; then
            echo "$out" | tail -2
            if grep -q "UPDATE_INCOMPATIBLE\|signatures do not match" <<<"$out"; then
                [[ $reinstall -eq 1 ]] || die "Installed app was signed on another computer. Re-run with --reinstall to uninstall it first (in-app settings are lost)."
                adb -s "$serial" uninstall "$package" >/dev/null
                adb -s "$serial" install "$apk" || die "APK install failed."
            else
                die "APK install failed."
            fi
        fi
        echo "APP INSTALL OK on $serial"
        ;;
    run)
        serial="$(phone_serial)"; need_flutter
        echo "Running on $serial. Use the phone screen; press q here to stop."
        cd "$app" && exec "$flutter" run --no-pub -d "$serial" "${defines[@]}"
        ;;
    info) info ;;
    screenshot)
        [[ "$shot" =~ ^[A-Za-z0-9_-]+\.png$ ]] || die "Use a plain file name like demo-1.png"
        serial="$(phone_serial)"; mkdir -p "$SAEMS_LOCAL"
        adb -s "$serial" exec-out screencap -p > "$SAEMS_LOCAL/$shot"
        echo "Saved .local/$shot"
        ;;
    *) echo "Unknown action: $action" >&2; usage 1 ;;
esac
