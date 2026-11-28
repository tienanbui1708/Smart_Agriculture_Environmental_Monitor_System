/// Automatic watering settings, shared by the server API and the device.
///
/// Watering is pulse based: when soil moisture drops below [thresholdPct]
/// the pump runs for at most [maxRuntimeSec] (the firmware also caps one run
/// at its relay safety limit, 60 s by default), then waits [minRestSec]
/// before another automatic run. A running automatic pulse stops early once
/// the soil reaches [thresholdPct] + 5 %.
///
/// Server JSON: `auto_watering` object with `enabled`,
/// `soil_moisture_threshold_pct`, `pump_max_runtime_sec`, `pump_min_rest_sec`.
/// Device JSON (BLE/AP): `irrigation` array
/// `[enabled 0/1, threshold %, max runtime s, min rest s]`.
class AutoWatering {
  const AutoWatering({
    this.enabled = false,
    this.thresholdPct = 30,
    this.maxRuntimeSec = 30,
    this.minRestSec = 1800,
  });

  final bool enabled;
  final int thresholdPct;
  final int maxRuntimeSec;
  final int minRestSec;

  static AutoWatering? fromServerJson(Object? json) {
    if (json is! Map) return null;
    return AutoWatering(
      enabled: json['enabled'] == true,
      thresholdPct: _int(json['soil_moisture_threshold_pct']) ?? 30,
      maxRuntimeSec: _int(json['pump_max_runtime_sec']) ?? 30,
      minRestSec: _int(json['pump_min_rest_sec']) ?? 1800,
    );
  }

  static AutoWatering? fromDeviceArray(Object? json) {
    if (json is! List || json.length != 4) return null;
    final values = json.map(_int).toList();
    if (values.contains(null)) return null;
    return AutoWatering(
      enabled: values[0] == 1,
      thresholdPct: values[1]!,
      maxRuntimeSec: values[2]!,
      minRestSec: values[3]!,
    );
  }

  Map<String, dynamic> toServerJson() => {
    'enabled': enabled,
    'soil_moisture_threshold_pct': thresholdPct,
    'pump_max_runtime_sec': maxRuntimeSec,
    'pump_min_rest_sec': minRestSec,
  };

  List<int> toDeviceArray() => [
    enabled ? 1 : 0,
    thresholdPct,
    maxRuntimeSec,
    minRestSec,
  ];

  /// Same limits as firmware irrigation.h; returns a message or null.
  String? validate() {
    if (thresholdPct < 1 || thresholdPct > 95) {
      return 'Ngưỡng độ ẩm đất để tưới phải từ 1 đến 95 %.';
    }
    if (maxRuntimeSec < 1 || maxRuntimeSec > 3600) {
      return 'Thời gian bơm mỗi lần phải từ 1 đến 3600 giây.';
    }
    if (minRestSec < 0 || minRestSec > 86400) {
      return 'Thời gian nghỉ giữa hai lần tưới phải từ 0 đến 1440 phút.';
    }
    return null;
  }

  @override
  bool operator ==(Object other) =>
      other is AutoWatering &&
      other.enabled == enabled &&
      other.thresholdPct == thresholdPct &&
      other.maxRuntimeSec == maxRuntimeSec &&
      other.minRestSec == minRestSec;

  @override
  int get hashCode =>
      Object.hash(enabled, thresholdPct, maxRuntimeSec, minRestSec);
}

/// Vietnamese label for telemetry `pump_reason`.
String pumpReasonLabel(String? reason) => switch (reason) {
  'auto' => 'tưới tự động',
  'manual' => 'điều khiển tay',
  'safety_off' => 'ngắt an toàn (hết thời gian tối đa)',
  'low_battery' => 'ngắt vì pin cạn',
  _ => '',
};

/// Vietnamese label for telemetry `battery_state`.
String batteryStateLabel(String? state) => switch (state) {
  'normal' => 'bình thường',
  'low' => 'pin yếu – đang tiết kiệm điện',
  'critical' => 'pin cạn – đã khóa máy bơm',
  _ => '',
};

int? _int(Object? value) {
  if (value is int) return value;
  if (value is num && value == value.roundToDouble()) return value.toInt();
  return int.tryParse(value?.toString() ?? '');
}
