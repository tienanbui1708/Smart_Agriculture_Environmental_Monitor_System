import 'package:flutter_test/flutter_test.dart';
import 'package:saems_app/src/models/device_models.dart';
import 'package:saems_app/src/models/telemetry.dart';
import 'package:saems_app/src/models/thresholds.dart';

void main() {
  group('ThresholdConfig', () {
    test('device array round trip keeps firmware order', () {
      final config = ThresholdConfig.fromDeviceArray([
        10.0, 35.0, null, 90, 35, null, null, 20000, 20,
      ]);
      expect(config.of('temperature_c').min, 10);
      expect(config.of('temperature_c').max, 35);
      expect(config.of('humidity_pct').max, 90);
      expect(config.of('soil_moisture_pct').min, 35);
      expect(config.of('light_lux').max, 20000);
      expect(config.of('battery_pct').min, 20);
      expect(config.toDeviceArray(), [
        10.0, 35.0, null, 90.0, 35.0, null, null, 20000.0, 20.0,
      ]);
    });

    test('wrong array length means no limits', () {
      expect(
        ThresholdConfig.fromDeviceArray([1, 2]).toDeviceArray(),
        List<double?>.filled(9, null),
      );
    });

    test('server JSON round trip and one-decimal rounding', () {
      final config = ThresholdConfig.fromServerJson({
        'temperature_c': {'min': null, 'max': 30.04},
        'battery_pct': {'min': 15},
      });
      expect(config.toDeviceArray()[1], 30.0);
      final json = config.toServerJson();
      expect(json['battery_pct'], {'min': 15.0});
      expect((json['soil_moisture_pct'] as Map)['max'], isNull);
    });

    test('validation reports range and ordering errors', () {
      expect(ThresholdConfig.empty().validate(), isNull);
      expect(
        const ThresholdConfig({
          'humidity_pct': MetricLimits(min: 80, max: 40),
        }).validate(),
        contains('nhỏ hơn'),
      );
      expect(
        const ThresholdConfig({
          'temperature_c': MetricLimits(max: 120),
        }).validate(),
        contains('-40..85'),
      );
    });
  });

  test('alarm mask decodes in firmware bit order', () {
    final labels = alarmsFromMask(0x002 | 0x010 | 0x100)
        .map((alarm) => alarm.label)
        .toList();
    expect(labels, ['Nhiệt độ cao', 'Độ ẩm đất thấp', 'Pin thấp']);
    expect(alarmsFromMask(0), isEmpty);
    expect(alarmsFromMask(null), isEmpty);
  });

  test('telemetry and device config read new firmware fields', () {
    final telemetry = Telemetry.fromJson({
      'device_id': 'farm-01',
      'timestamp': '2026-11-07T08:00:00Z',
      'config_version': 3,
      'alarm_mask': 2,
    });
    expect(telemetry.configVersion, 3);
    expect(telemetry.alarmMask, 2);
    expect(telemetry.toJson()['alarm_mask'], 2);

    final legacy = Telemetry.fromJson({
      'device_id': 'farm-01',
      'timestamp': '2026-11-07T08:00:00Z',
    });
    expect(legacy.alarmMask, 0);
    expect(legacy.configVersion, isNull);

    final config = DeviceConfig.fromJson({
      'device_id': 'farm-01',
      'config_version': 0,
      'thresholds': [null, 30.0, null, null, null, null, null, null, null],
    });
    expect(config.thresholds.of('temperature_c').max, 30);
    expect(config.toJson()['thresholds'], hasLength(9));
  });

  test('server configuration parses sync state', () {
    final configuration = ServerConfiguration.fromJson({
      'device_id': 'farm-01',
      'version': 4,
      'applied_version': 3,
      'sync_state': 'pending',
      'configuration': {
        'telemetry_interval_s': 300,
        'power_mode': null,
        'offline_alert_after_s': null,
        'thresholds': {
          'temperature_c': {'min': 10, 'max': 35},
        },
      },
    });
    expect(configuration.telemetryIntervalSeconds, 300);
    expect(configuration.powerMode, isNull);
    expect(configuration.thresholds.of('temperature_c').min, 10);
    expect(configuration.syncLabel, contains('Đang chờ'));
    expect(configuration.toRequestJson()['telemetry_interval_s'], 300);
  });

  test('alert parses state and backlog', () {
    final alert = DeviceAlert.fromJson({
      'id': 7,
      'device_id': 'farm-01',
      'kind': 'threshold',
      'metric': 'temperature_c',
      'level': 'high',
      'state': 'active',
      'message': 'Nhiệt độ cao',
      'created_at': '2026-11-07T08:00:00+00:00',
      'backlog': true,
    });
    expect(alert.active, isTrue);
    expect(alert.backlog, isTrue);
    expect(alert.createdAt.isUtc, isTrue);
  });
}
