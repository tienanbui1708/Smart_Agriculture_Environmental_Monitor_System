import 'package:flutter_test/flutter_test.dart';
import 'package:saems_app/src/models/device_models.dart';
import 'package:saems_app/src/models/telemetry.dart';
import 'package:saems_app/src/models/thresholds.dart';
import 'package:saems_app/src/models/watering.dart';

void main() {
  test('device array round trip matches firmware irrigation.h', () {
    const watering = AutoWatering(
      enabled: true,
      thresholdPct: 35,
      maxRuntimeSec: 20,
      minRestSec: 900,
    );
    expect(watering.toDeviceArray(), [1, 35, 20, 900]);
    expect(AutoWatering.fromDeviceArray([1, 35, 20, 900]), watering);
    expect(AutoWatering.fromDeviceArray([1, 35, 20]), isNull);
    expect(AutoWatering.fromDeviceArray(null), isNull);
  });

  test('server JSON uses contract names', () {
    final watering = AutoWatering.fromServerJson({
      'enabled': true,
      'soil_moisture_threshold_pct': 40,
      'pump_max_runtime_sec': 15,
      'pump_min_rest_sec': 600,
    })!;
    expect(watering.toServerJson(), {
      'enabled': true,
      'soil_moisture_threshold_pct': 40,
      'pump_max_runtime_sec': 15,
      'pump_min_rest_sec': 600,
    });
    expect(AutoWatering.fromServerJson(null), isNull);
  });

  test('validation follows firmware limits', () {
    expect(const AutoWatering().validate(), isNull);
    expect(const AutoWatering(thresholdPct: 0).validate(), isNotNull);
    expect(const AutoWatering(thresholdPct: 96).validate(), isNotNull);
    expect(const AutoWatering(maxRuntimeSec: 3601).validate(), isNotNull);
    expect(const AutoWatering(minRestSec: 86401).validate(), isNotNull);
  });

  test('server configuration PUT always carries auto_watering', () {
    final unmanaged = ServerConfiguration.fromJson({
      'device_id': 'farm-01',
      'version': 3,
      'sync_state': 'applied',
      'configuration': {'thresholds': <String, dynamic>{}},
    });
    expect(unmanaged.autoWatering, isNull);
    expect(unmanaged.toRequestJson().containsKey('auto_watering'), isTrue);
    expect(unmanaged.toRequestJson()['auto_watering'], isNull);

    final managed = ServerConfiguration.fromJson({
      'device_id': 'farm-01',
      'version': 4,
      'sync_state': 'pending',
      'configuration': {
        'auto_watering': {
          'enabled': false,
          'soil_moisture_threshold_pct': 30,
          'pump_max_runtime_sec': 30,
          'pump_min_rest_sec': 1800,
        },
      },
    });
    expect(managed.toRequestJson()['auto_watering'], {
      'enabled': false,
      'soil_moisture_threshold_pct': 30,
      'pump_max_runtime_sec': 30,
      'pump_min_rest_sec': 1800,
    });
  });

  test('device config and telemetry parse phase B fields', () {
    final config = DeviceConfig.fromJson({
      'device_id': 'farm-01',
      'irrigation': [0, 30, 30, 1800],
    });
    expect(config.autoWatering, const AutoWatering());
    expect(config.toJson()['irrigation'], [0, 30, 30, 1800]);

    final telemetry = Telemetry.fromJson({
      'device_id': 'farm-01',
      'battery_pct': 15.5,
      'battery_source': 'simulated',
      'battery_state': 'low',
      'pump_state': 'off',
      'pump_reason': 'safety_off',
    });
    expect(telemetry.batteryPct, 15.5);
    expect(telemetry.batteryState, 'low');
    expect(pumpReasonLabel(telemetry.pumpReason), contains('an toàn'));
    expect(batteryStateLabel('critical'), contains('khóa máy bơm'));
    expect(Telemetry.fromJson(telemetry.toJson()).pumpReason, 'safety_off');
    expect(telemetry.queueDropped, 0);
    expect(
      Telemetry.fromJson({'device_id': 'farm-01', 'queue_dropped': 12})
          .queueDropped,
      12,
    );
  });
}
