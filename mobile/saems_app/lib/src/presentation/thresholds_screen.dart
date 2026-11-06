import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../data/direct_config_transport.dart';
import '../data/remote_config_transport.dart';
import '../models/thresholds.dart';
import '../state/app_state.dart';

/// Edit alarm limits, sampling interval and power mode.
///
/// With the Server source the values are stored on the server and pushed to
/// the device over MQTT (retained). With BLE / Wi-Fi AP they are written to
/// the device directly; the server configuration re-applies when the device
/// reconnects, so changes meant to last should also be saved on the server.
class ThresholdsScreen extends ConsumerStatefulWidget {
  const ThresholdsScreen({super.key});

  @override
  ConsumerState<ThresholdsScreen> createState() => _ThresholdsScreenState();
}

class _ThresholdsScreenState extends ConsumerState<ThresholdsScreen> {
  final _interval = TextEditingController();
  final _offlineAfter = TextEditingController();
  final Map<String, TextEditingController> _min = {
    for (final metric in thresholdMetrics) metric.key: TextEditingController(),
  };
  final Map<String, TextEditingController> _max = {
    for (final metric in thresholdMetrics)
      if (metric.hasMax) metric.key: TextEditingController(),
  };
  String? _powerMode;
  bool _busy = false;
  bool _loaded = false;
  ServerConfiguration? _server;
  String _message = '';

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _load());
  }

  @override
  void dispose() {
    for (final controller in [
      _interval,
      _offlineAfter,
      ..._min.values,
      ..._max.values,
    ]) {
      controller.dispose();
    }
    super.dispose();
  }

  Object get _transport => ref.read(telemetryRepositoryProvider).transport;
  bool get _isServer => _transport is RemoteConfigTransport;

  void _fill(ThresholdConfig thresholds, int? interval, String? powerMode) {
    for (final metric in thresholdMetrics) {
      _min[metric.key]!.text = _format(thresholds.of(metric.key).min);
      _max[metric.key]?.text = _format(thresholds.of(metric.key).max);
    }
    _interval.text = interval?.toString() ?? '';
    _powerMode = powerMode;
  }

  Future<void> _load() async {
    setState(() => _busy = true);
    final deviceId = ref.read(currentSettingsProvider).deviceId;
    try {
      final transport = _transport;
      if (transport is RemoteConfigTransport) {
        final server = await transport.readServerConfiguration(deviceId);
        _server = server;
        _fill(
          server.thresholds,
          server.telemetryIntervalSeconds,
          server.powerMode,
        );
        _offlineAfter.text = server.offlineAlertAfterSeconds?.toString() ?? '';
        _message = server.syncLabel;
      } else if (transport is DirectConfigTransport) {
        await ref.read(telemetryRepositoryProvider).transport.connect();
        final json = await transport.readDeviceConfig();
        _fill(
          ThresholdConfig.fromDeviceArray(json['thresholds']),
          int.tryParse(json['telemetry_interval_s']?.toString() ?? ''),
          json['power_mode']?.toString(),
        );
        final version = json['config_version'];
        _message = version == 0 || version == null
            ? 'Thiết bị đang dùng cấu hình cục bộ.'
            : 'Thiết bị đang dùng cấu hình server phiên bản $version.';
      } else {
        _message =
            'Chọn nguồn dữ liệu Server, BLE hoặc Wi-Fi AP trong Cài đặt để chỉnh ngưỡng.';
      }
      _loaded = transport is RemoteConfigTransport ||
          transport is DirectConfigTransport;
    } catch (error) {
      _message = 'Không đọc được cấu hình: $error';
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  ThresholdConfig _readThresholds() => ThresholdConfig({
    for (final metric in thresholdMetrics)
      metric.key: MetricLimits(
        min: _parse(_min[metric.key]!.text, metric.label),
        max: metric.hasMax ? _parse(_max[metric.key]!.text, metric.label) : null,
      ),
  });

  Future<void> _save() async {
    setState(() => _busy = true);
    final deviceId = ref.read(currentSettingsProvider).deviceId;
    try {
      final thresholds = _readThresholds();
      final error = thresholds.validate();
      if (error != null) throw FormatException(error);
      final interval = _interval.text.trim().isEmpty
          ? null
          : int.tryParse(_interval.text.trim());
      if (_interval.text.trim().isNotEmpty &&
          (interval == null || interval < 1 || interval > 3600)) {
        throw const FormatException('Chu kỳ phải từ 1 đến 3600 giây.');
      }
      final transport = _transport;
      if (transport is RemoteConfigTransport) {
        final offline = _offlineAfter.text.trim().isEmpty
            ? null
            : int.tryParse(_offlineAfter.text.trim());
        if (_offlineAfter.text.trim().isNotEmpty &&
            (offline == null || offline < 60 || offline > 86400)) {
          throw const FormatException(
            'Thời gian báo mất kết nối phải từ 60 đến 86400 giây.',
          );
        }
        final saved = await transport.writeServerConfiguration(
          deviceId,
          ServerConfiguration(
            deviceId: deviceId,
            version: _server?.version ?? 0,
            syncState: 'unknown',
            thresholds: thresholds,
            telemetryIntervalSeconds: interval,
            powerMode: _powerMode,
            offlineAlertAfterSeconds: offline,
          ),
        );
        _server = saved;
        _message =
            'Đã lưu trên server (phiên bản ${saved.version}). '
            'Thiết bị áp dụng ở chu kỳ lấy mẫu kế tiếp khi kết nối MQTT.';
      } else if (transport is DirectConfigTransport) {
        await transport.writeDeviceConfig({
          'thresholds': thresholds.toDeviceArray(),
          'telemetry_interval_s': ?interval,
          'power_mode': ?_powerMode,
        });
        _message =
            'Đã lưu vào ESP32. Khi thiết bị nối lại server, cấu hình trên '
            'server (nếu có) sẽ được áp dụng lại.';
      }
      ref.read(diagnosticsProvider.notifier).add('Lưu ngưỡng: $_message');
    } catch (error) {
      _message = error.toString();
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Widget _number(TextEditingController controller, String label) => Expanded(
    child: TextField(
      controller: controller,
      enabled: !_busy,
      keyboardType: const TextInputType.numberWithOptions(
        signed: true,
        decimal: true,
      ),
      decoration: InputDecoration(labelText: label, hintText: 'tắt'),
    ),
  );

  @override
  Widget build(BuildContext context) {
    final isServer = _isServer;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Ngưỡng & chu kỳ'),
        actions: [
          IconButton(
            tooltip: 'Đọc lại',
            onPressed: _busy ? null : _load,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Card(
            child: ListTile(
              leading: Icon(isServer ? Icons.cloud_outlined : Icons.link),
              title: Text(isServer ? 'Lưu trên server' : 'Ghi trực tiếp vào ESP32'),
              subtitle: Text(_message),
            ),
          ),
          if (_busy) const LinearProgressIndicator(),
          const SizedBox(height: 8),
          const Text('Để trống một ô để tắt ngưỡng đó.'),
          const SizedBox(height: 8),
          for (final metric in thresholdMetrics)
            Padding(
              padding: const EdgeInsets.only(bottom: 12),
              child: Row(
                children: [
                  SizedBox(
                    width: 110,
                    child: Text('${metric.label}\n(${metric.unit})'),
                  ),
                  _number(_min[metric.key]!, 'Thấp nhất'),
                  if (metric.hasMax) ...[
                    const SizedBox(width: 12),
                    _number(_max[metric.key]!, 'Cao nhất'),
                  ],
                ],
              ),
            ),
          const Divider(),
          TextField(
            controller: _interval,
            enabled: !_busy,
            keyboardType: TextInputType.number,
            decoration: InputDecoration(
              labelText: 'Chu kỳ lấy mẫu (giây)',
              hintText: isServer ? 'để trống: không quản lý' : 'giữ nguyên',
            ),
          ),
          const SizedBox(height: 12),
          DropdownButtonFormField<String?>(
            key: ValueKey(_powerMode),
            initialValue: _powerMode,
            decoration: const InputDecoration(labelText: 'Chế độ năng lượng'),
            items: [
              DropdownMenuItem(
                value: null,
                child: Text(isServer ? 'Không quản lý' : 'Giữ nguyên'),
              ),
              const DropdownMenuItem(value: 'direct', child: Text('Direct')),
              const DropdownMenuItem(value: 'light', child: Text('Light Sleep')),
              const DropdownMenuItem(
                value: 'battery',
                child: Text('Battery – Deep Sleep'),
              ),
            ],
            onChanged: _busy ? null : (value) => setState(() => _powerMode = value),
          ),
          if (isServer) ...[
            const SizedBox(height: 12),
            TextField(
              controller: _offlineAfter,
              enabled: !_busy,
              keyboardType: TextInputType.number,
              decoration: const InputDecoration(
                labelText: 'Báo mất kết nối sau (giây)',
                hintText: 'mặc định: 3 × chu kỳ + 60, tối thiểu 300',
              ),
            ),
          ],
          const SizedBox(height: 16),
          FilledButton.icon(
            onPressed: _busy || !_loaded ? null : _save,
            icon: const Icon(Icons.save_outlined),
            label: const Text('Lưu'),
          ),
        ],
      ),
    );
  }
}

String _format(double? value) {
  if (value == null) return '';
  return value == value.roundToDouble()
      ? value.toInt().toString()
      : value.toStringAsFixed(1);
}

double? _parse(String text, String label) {
  final trimmed = text.trim().replaceAll(',', '.');
  if (trimmed.isEmpty) return null;
  final value = double.tryParse(trimmed);
  if (value == null) throw FormatException('$label: "$text" không phải số.');
  return value;
}
