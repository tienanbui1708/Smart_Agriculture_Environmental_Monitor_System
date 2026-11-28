import '../models/thresholds.dart';

/// Server-managed configuration and alert history (server data source only).
abstract interface class RemoteConfigTransport {
  Future<ServerConfiguration> readServerConfiguration(String deviceId);
  Future<ServerConfiguration> writeServerConfiguration(
    String deviceId,
    ServerConfiguration configuration,
  );
  Future<List<DeviceAlert>> readAlerts(
    String deviceId, {
    bool activeOnly = false,
    int limit = 50,
  });
}
