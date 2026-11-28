import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../data/remote_config_transport.dart';
import '../models/thresholds.dart';
import '../state/app_state.dart';

/// Alert history from the server, or the device's own alarm flags when the
/// app is connected directly (BLE / Wi-Fi AP) without the server.
class AlertsScreen extends ConsumerStatefulWidget {
  const AlertsScreen({super.key});

  @override
  ConsumerState<AlertsScreen> createState() => _AlertsScreenState();
}

class _AlertsScreenState extends ConsumerState<AlertsScreen> {
  bool _activeOnly = false;
  late Future<List<DeviceAlert>>? _alerts = _load();

  Future<List<DeviceAlert>>? _load() {
    final transport = ref.read(telemetryRepositoryProvider).transport;
    if (transport is! RemoteConfigTransport) return null;
    return (transport as RemoteConfigTransport).readAlerts(
      ref.read(currentSettingsProvider).deviceId,
      activeOnly: _activeOnly,
    );
  }

  void _reload() => setState(() => _alerts = _load());

  @override
  Widget build(BuildContext context) {
    final alerts = _alerts;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Cảnh báo'),
        actions: [
          if (alerts != null)
            IconButton(
              tooltip: 'Tải lại',
              onPressed: _reload,
              icon: const Icon(Icons.refresh),
            ),
        ],
      ),
      body: alerts == null ? const _DeviceAlarms() : _serverList(alerts),
    );
  }

  Widget _serverList(Future<List<DeviceAlert>> alerts) => Column(
    children: [
      SwitchListTile(
        title: const Text('Chỉ cảnh báo đang hoạt động'),
        value: _activeOnly,
        onChanged: (value) {
          _activeOnly = value;
          _reload();
        },
      ),
      Expanded(
        child: FutureBuilder<List<DeviceAlert>>(
          future: alerts,
          builder: (context, snapshot) {
            if (snapshot.connectionState != ConnectionState.done) {
              return const Center(child: CircularProgressIndicator());
            }
            if (snapshot.hasError) {
              return Center(
                child: Padding(
                  padding: const EdgeInsets.all(24),
                  child: Text('Không tải được cảnh báo: ${snapshot.error}'),
                ),
              );
            }
            final items = snapshot.data ?? const [];
            if (items.isEmpty) {
              return const Center(child: Text('Chưa có cảnh báo.'));
            }
            return RefreshIndicator(
              onRefresh: () async {
                _reload();
                await _alerts;
              },
              child: ListView.separated(
                itemCount: items.length,
                separatorBuilder: (_, _) => const Divider(height: 1),
                itemBuilder: (context, index) => _AlertTile(items[index]),
              ),
            );
          },
        ),
      ),
    ],
  );
}

class _AlertTile extends StatelessWidget {
  const _AlertTile(this.alert);

  final DeviceAlert alert;

  @override
  Widget build(BuildContext context) {
    final color = alert.active
        ? Theme.of(context).colorScheme.error
        : Theme.of(context).colorScheme.outline;
    final created = alert.createdAt.toLocal();
    return ListTile(
      leading: Icon(
        alert.kind == 'connectivity'
            ? Icons.wifi_off
            : Icons.warning_amber_rounded,
        color: color,
      ),
      title: Text(alert.message),
      subtitle: Text(
        '${alert.active ? 'Đang hoạt động' : 'Đã kết thúc'} · '
        '${_two(created.day)}/${_two(created.month)} '
        '${_two(created.hour)}:${_two(created.minute)}'
        '${alert.backlog ? ' · dữ liệu gửi bù' : ''}',
      ),
    );
  }
}

class _DeviceAlarms extends ConsumerWidget {
  const _DeviceAlarms();

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final telemetry = ref.watch(telemetryProvider);
    return telemetry.when(
      loading: () => const Center(child: CircularProgressIndicator()),
      error: (error, _) => Center(child: Text('Chưa có dữ liệu: $error')),
      data: (reading) {
        final alarms = alarmsFromMask(reading.value.alarmMask);
        return ListView(
          padding: const EdgeInsets.all(16),
          children: [
            const Card(
              child: ListTile(
                leading: Icon(Icons.info_outline),
                title: Text('Cảnh báo tại thiết bị'),
                subtitle: Text(
                  'Kết nối trực tiếp: hiển thị cờ cảnh báo ESP32 tự tính theo '
                  'ngưỡng đang lưu trong thiết bị. Lịch sử cảnh báo và Telegram '
                  'cần nguồn dữ liệu Server.',
                ),
              ),
            ),
            if (alarms.isEmpty)
              const ListTile(
                leading: Icon(Icons.check_circle_outline, color: Colors.green),
                title: Text('Không có ngưỡng nào bị vượt.'),
              ),
            for (final alarm in alarms)
              ListTile(
                leading: Icon(
                  Icons.warning_amber_rounded,
                  color: Theme.of(context).colorScheme.error,
                ),
                title: Text(alarm.label),
              ),
          ],
        );
      },
    );
  }
}

String _two(int value) => value.toString().padLeft(2, '0');
