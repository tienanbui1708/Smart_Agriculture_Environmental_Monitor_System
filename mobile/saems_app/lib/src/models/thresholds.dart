import 'watering.dart';

/// Alarm limits shared by server, firmware and app.
///
/// The device array order and the telemetry `alarm_mask` bits follow
/// firmware `threshold_monitor.h`:
/// `[temp_min, temp_max, hum_min, hum_max, soil_min, soil_max,
///   light_min, light_max, battery_min]`.
class ThresholdMetric {
  const ThresholdMetric({
    required this.key,
    required this.label,
    required this.unit,
    required this.lowest,
    required this.highest,
    this.hasMax = true,
  });

  final String key;
  final String label;
  final String unit;
  final double lowest;
  final double highest;
  final bool hasMax;
}

const thresholdMetrics = [
  ThresholdMetric(
    key: 'temperature_c',
    label: 'Nhiệt độ',
    unit: '°C',
    lowest: -40,
    highest: 85,
  ),
  ThresholdMetric(
    key: 'humidity_pct',
    label: 'Độ ẩm không khí',
    unit: '%',
    lowest: 0,
    highest: 100,
  ),
  ThresholdMetric(
    key: 'soil_moisture_pct',
    label: 'Độ ẩm đất',
    unit: '%',
    lowest: 0,
    highest: 100,
  ),
  ThresholdMetric(
    key: 'light_lux',
    label: 'Ánh sáng',
    unit: 'lux',
    lowest: 0,
    highest: 100000,
  ),
  ThresholdMetric(
    key: 'battery_pct',
    label: 'Pin',
    unit: '%',
    lowest: 0,
    highest: 100,
    hasMax: false,
  ),
];

class MetricLimits {
  const MetricLimits({this.min, this.max});

  final double? min;
  final double? max;
}

class ThresholdConfig {
  const ThresholdConfig(this.limits);

  factory ThresholdConfig.empty() => const ThresholdConfig({});

  /// Keyed by [ThresholdMetric.key]; a missing key means no limits.
  final Map<String, MetricLimits> limits;

  MetricLimits of(String key) => limits[key] ?? const MetricLimits();

  factory ThresholdConfig.fromServerJson(Object? json) {
    final map = json is Map ? json : const {};
    return ThresholdConfig({
      for (final metric in thresholdMetrics)
        if (map[metric.key] is Map)
          metric.key: MetricLimits(
            min: _number((map[metric.key] as Map)['min']),
            max: metric.hasMax
                ? _number((map[metric.key] as Map)['max'])
                : null,
          ),
    });
  }

  Map<String, dynamic> toServerJson() => {
    for (final metric in thresholdMetrics)
      metric.key: {
        'min': of(metric.key).min,
        if (metric.hasMax) 'max': of(metric.key).max,
      },
  };

  factory ThresholdConfig.fromDeviceArray(Object? value) {
    final values = value is List ? value : const [];
    if (values.length != 9) return ThresholdConfig.empty();
    final result = <String, MetricLimits>{};
    var index = 0;
    for (final metric in thresholdMetrics) {
      final low = _number(values[index++]);
      final high = metric.hasMax ? _number(values[index++]) : null;
      result[metric.key] = MetricLimits(min: low, max: high);
    }
    return ThresholdConfig(result);
  }

  List<double?> toDeviceArray() => [
    for (final metric in thresholdMetrics) ...[
      _round(of(metric.key).min),
      if (metric.hasMax) _round(of(metric.key).max),
    ],
  ];

  /// Returns a Vietnamese error message, or null when every limit is valid.
  String? validate() {
    for (final metric in thresholdMetrics) {
      final limits = of(metric.key);
      for (final value in [limits.min, limits.max]) {
        if (value != null && (value < metric.lowest || value > metric.highest)) {
          return '${metric.label}: giá trị phải trong khoảng '
              '${_text(metric.lowest)}..${_text(metric.highest)} ${metric.unit}.';
        }
      }
      if (limits.min != null &&
          limits.max != null &&
          !(limits.min! < limits.max!)) {
        return '${metric.label}: ngưỡng thấp phải nhỏ hơn ngưỡng cao.';
      }
    }
    return null;
  }
}

/// One active alarm bit decoded from telemetry `alarm_mask`.
class ActiveAlarm {
  const ActiveAlarm(this.metric, this.isHigh);

  final ThresholdMetric metric;
  final bool isHigh;

  String get label =>
      '${metric.label} ${isHigh ? 'cao' : 'thấp'}';
}

List<ActiveAlarm> alarmsFromMask(int? mask) {
  if (mask == null || mask == 0) return const [];
  final alarms = <ActiveAlarm>[];
  var bit = 0;
  for (final metric in thresholdMetrics) {
    if (mask & (1 << bit) != 0) alarms.add(ActiveAlarm(metric, false));
    bit++;
    if (metric.hasMax) {
      if (mask & (1 << bit) != 0) alarms.add(ActiveAlarm(metric, true));
      bit++;
    }
  }
  return alarms;
}

/// Server-side configuration plus its synchronisation state.
class ServerConfiguration {
  const ServerConfiguration({
    required this.deviceId,
    required this.version,
    required this.syncState,
    required this.thresholds,
    this.appliedVersion,
    this.telemetryIntervalSeconds,
    this.powerMode,
    this.offlineAlertAfterSeconds,
    this.autoWatering,
  });

  final String deviceId;
  final int version;
  /// null: the server does not manage automatic watering.
  final AutoWatering? autoWatering;
  final int? appliedVersion;
  final String syncState;
  final int? telemetryIntervalSeconds;
  final String? powerMode;
  final int? offlineAlertAfterSeconds;
  final ThresholdConfig thresholds;

  factory ServerConfiguration.fromJson(Map<String, dynamic> json) {
    final configuration = json['configuration'] is Map
        ? Map<String, dynamic>.from(json['configuration'] as Map)
        : const <String, dynamic>{};
    return ServerConfiguration(
      deviceId: json['device_id']?.toString() ?? '',
      version: _integer(json['version']) ?? 0,
      appliedVersion: _integer(json['applied_version']),
      syncState: json['sync_state']?.toString() ?? 'unknown',
      telemetryIntervalSeconds: _integer(configuration['telemetry_interval_s']),
      powerMode: configuration['power_mode']?.toString(),
      offlineAlertAfterSeconds: _integer(
        configuration['offline_alert_after_s'],
      ),
      thresholds: ThresholdConfig.fromServerJson(configuration['thresholds']),
      autoWatering: AutoWatering.fromServerJson(configuration['auto_watering']),
    );
  }

  /// PUT replaces the whole configuration, so every managed part is sent.
  Map<String, dynamic> toRequestJson() => {
    'telemetry_interval_s': telemetryIntervalSeconds,
    'power_mode': powerMode,
    'offline_alert_after_s': offlineAlertAfterSeconds,
    'thresholds': thresholds.toServerJson(),
    'auto_watering': autoWatering?.toServerJson(),
  };

  String get syncLabel => switch (syncState) {
    'applied' => 'Thiết bị đã áp dụng phiên bản $version',
    'pending' =>
      'Đang chờ thiết bị áp dụng phiên bản $version '
          '(thiết bị đang ở ${appliedVersion ?? '?'})',
    'not_configured' => 'Server chưa quản lý cấu hình thiết bị này',
    _ => 'Chưa có telemetry xác nhận phiên bản $version',
  };
}

class DeviceAlert {
  const DeviceAlert({
    required this.id,
    required this.deviceId,
    required this.kind,
    required this.metric,
    required this.level,
    required this.active,
    required this.message,
    required this.createdAt,
    this.resolvedAt,
    this.backlog = false,
  });

  final int id;
  final String deviceId;
  final String kind;
  final String metric;
  final String level;
  final bool active;
  final String message;
  final DateTime createdAt;
  final DateTime? resolvedAt;
  final bool backlog;

  factory DeviceAlert.fromJson(Map<String, dynamic> json) => DeviceAlert(
    id: _integer(json['id']) ?? 0,
    deviceId: json['device_id']?.toString() ?? '',
    kind: json['kind']?.toString() ?? 'threshold',
    metric: json['metric']?.toString() ?? '',
    level: json['level']?.toString() ?? '',
    active: json['state'] == 'active',
    message: json['message']?.toString() ?? '',
    createdAt:
        DateTime.tryParse(json['created_at']?.toString() ?? '')?.toUtc() ??
        DateTime.fromMillisecondsSinceEpoch(0, isUtc: true),
    resolvedAt: DateTime.tryParse(
      json['resolved_at']?.toString() ?? '',
    )?.toUtc(),
    backlog: json['backlog'] == true,
  );
}

double? _number(Object? value) {
  if (value == null || value is bool) return null;
  if (value is num) return value.toDouble();
  return double.tryParse(value.toString());
}

int? _integer(Object? value) {
  if (value is int) return value;
  return int.tryParse(value?.toString() ?? '');
}

double? _round(double? value) =>
    value == null ? null : (value * 10).roundToDouble() / 10;

String _text(double value) =>
    value == value.roundToDouble() ? value.toInt().toString() : '$value';
