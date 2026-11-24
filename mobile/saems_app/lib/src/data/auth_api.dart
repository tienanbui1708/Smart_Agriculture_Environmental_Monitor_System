import 'package:dio/dio.dart';

import '../core/app_exception.dart';
import 'dio_factory.dart';

class LoginResult {
  const LoginResult({
    required this.accessToken,
    required this.username,
    this.expiresAt,
  });

  final String accessToken;
  final String username;
  final DateTime? expiresAt;
}

/// Server login (`POST /auth/login`, `/auth/logout`). The returned bearer
/// token replaces the long-lived API token in secure storage.
class AuthApi {
  AuthApi(String baseUrl, {Dio? dio}) : _dio = dio ?? createDio(baseUrl);

  final Dio _dio;

  Future<LoginResult> login(String username, String password) async {
    try {
      final response = await _dio.post<Object?>(
        '/auth/login',
        data: {'username': username, 'password': password},
      );
      final json = jsonObject(response.data);
      final token = json['access_token']?.toString() ?? '';
      if (token.isEmpty) {
        throw const AppException(
          AppErrorKind.invalidData,
          'Server không trả token đăng nhập.',
        );
      }
      return LoginResult(
        accessToken: token,
        username: json['username']?.toString() ?? username,
        expiresAt: DateTime.tryParse(json['expires_at']?.toString() ?? ''),
      );
    } on DioException catch (error) {
      final status = error.response?.statusCode;
      if (status == 401) {
        throw const AppException(
          AppErrorKind.unauthorized,
          'Sai tên đăng nhập hoặc mật khẩu.',
          statusCode: 401,
        );
      }
      if (status == 429) {
        final wait = error.response?.headers.value('retry-after');
        throw AppException(
          AppErrorKind.unauthorized,
          'Đăng nhập sai quá nhiều lần. Thử lại sau ${wait ?? 'vài'} giây.',
          statusCode: 429,
        );
      }
      if (status == 503) {
        throw const AppException(
          AppErrorKind.unavailable,
          'Server chưa bật đăng nhập (chưa đặt mật khẩu admin).',
          statusCode: 503,
        );
      }
      mapDioError(error);
    }
  }

  /// Best effort: the local session is cleared even if this fails.
  Future<void> logout(String token) async {
    try {
      await _dio.post<Object?>(
        '/auth/logout',
        options: Options(headers: {'Authorization': 'Bearer $token'}),
      );
    } on DioException {
      // Offline or already expired: nothing to revoke.
    }
  }
}
