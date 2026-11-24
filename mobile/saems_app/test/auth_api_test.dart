import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:saems_app/src/core/app_exception.dart';
import 'package:saems_app/src/data/auth_api.dart';
import 'package:saems_app/src/data/dio_factory.dart';

Dio _fake(Response<Object?> Function(RequestOptions request) reply,
    {List<RequestOptions>? seen}) {
  final dio = Dio(BaseOptions(baseUrl: 'https://server.example'));
  dio.interceptors.add(
    InterceptorsWrapper(
      onRequest: (request, handler) {
        seen?.add(request);
        final response = reply(request);
        if ((response.statusCode ?? 200) >= 400) {
          handler.reject(
            DioException(
              requestOptions: request,
              response: response,
              type: DioExceptionType.badResponse,
            ),
          );
        } else {
          handler.resolve(response);
        }
      },
    ),
  );
  return dio;
}

void main() {
  test('login returns the session token and never sends it back', () async {
    final seen = <RequestOptions>[];
    final api = AuthApi(
      'https://server.example',
      dio: _fake(
        (request) => Response(
          requestOptions: request,
          statusCode: 200,
          data: {
            'access_token': 'session-abc',
            'token_type': 'bearer',
            'expires_at': '2026-12-24T00:00:00+00:00',
            'username': 'admin',
          },
        ),
        seen: seen,
      ),
    );
    final result = await api.login('admin', 'pw');
    expect(result.accessToken, 'session-abc');
    expect(result.username, 'admin');
    expect(result.expiresAt, isNotNull);
    expect(seen.single.path, '/auth/login');
    expect(seen.single.data, {'username': 'admin', 'password': 'pw'});
  });

  test('login maps 401, 429 and 503 to readable errors', () async {
    Future<AppException> failWith(int status, [Map<String, List<String>>? headers]) async {
      final api = AuthApi(
        'https://server.example',
        dio: _fake(
          (request) => Response(
            requestOptions: request,
            statusCode: status,
            headers: Headers.fromMap(headers ?? {}),
          ),
        ),
      );
      try {
        await api.login('admin', 'bad');
      } on AppException catch (error) {
        return error;
      }
      throw StateError('expected failure');
    }

    expect((await failWith(401)).message, contains('Sai tên đăng nhập'));
    expect(
      (await failWith(429, {
        'retry-after': ['120'],
      })).message,
      contains('120'),
    );
    expect((await failWith(503)).kind, AppErrorKind.unavailable);
  });

  test('logout sends the session token and tolerates failures', () async {
    final seen = <RequestOptions>[];
    final api = AuthApi(
      'https://server.example',
      dio: _fake(
        (request) => Response(requestOptions: request, statusCode: 500),
        seen: seen,
      ),
    );
    await api.logout('session-abc');
    expect(seen.single.headers['Authorization'], 'Bearer session-abc');
  });

  test('device token is sent as bearer and AP 401 asks for the device code', () {
    final dio = createDio('http://192.168.4.1', token: 'ap-password-123');
    expect(dio.options.headers['Authorization'], 'Bearer ap-password-123');

    AppException mapped(String baseUrl) {
      final request = RequestOptions(baseUrl: baseUrl, path: '/v1/config');
      try {
        mapDioError(
          DioException(
            requestOptions: request,
            response: Response(requestOptions: request, statusCode: 401),
            type: DioExceptionType.badResponse,
          ),
        );
      } on AppException catch (error) {
        return error;
      }
    }

    expect(mapped('http://192.168.4.1').message, contains('mã thiết bị'));
    expect(
      mapped('https://server.example').message,
      contains('đăng nhập lại'),
    );
  });
}
