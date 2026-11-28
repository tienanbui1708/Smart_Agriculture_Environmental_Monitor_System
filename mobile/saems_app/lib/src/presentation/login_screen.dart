import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../core/app_exception.dart';
import '../data/auth_api.dart';
import '../state/app_state.dart';
import 'settings_screen.dart';

/// Server login. The session token is kept in secure storage and sent as a
/// bearer token by the server transport; the password is never stored.
class LoginScreen extends ConsumerStatefulWidget {
  const LoginScreen({super.key, this.embedded = false});

  /// true when shown instead of the main shell (no back navigation).
  final bool embedded;

  @override
  ConsumerState<LoginScreen> createState() => _LoginScreenState();
}

class _LoginScreenState extends ConsumerState<LoginScreen> {
  final _formKey = GlobalKey<FormState>();
  final _user = TextEditingController(text: 'admin');
  final _password = TextEditingController();
  bool _busy = false;
  bool _showPassword = false;
  String? _error;

  @override
  void dispose() {
    _user.dispose();
    _password.dispose();
    super.dispose();
  }

  Future<void> _login() async {
    if (!_formKey.currentState!.validate()) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    final settings = ref.read(currentSettingsProvider);
    try {
      final result = await AuthApi(
        settings.apiBaseUrl,
      ).login(_user.text.trim(), _password.text);
      await ref
          .read(settingsStoreProvider)
          .saveSession(result.accessToken, result.username);
      _password.clear();
      ref.invalidate(apiTokenProvider);
      ref.invalidate(sessionUserProvider);
      ref.invalidate(telemetryProvider);
      ref.invalidate(historyProvider);
      ref
          .read(diagnosticsProvider.notifier)
          .add('Đăng nhập server: ${result.username}');
      if (mounted && !widget.embedded) Navigator.of(context).pop(true);
    } on AppException catch (error) {
      setState(() => _error = error.message);
    } catch (error) {
      setState(() => _error = 'Không đăng nhập được: $error');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final server = ref.watch(currentSettingsProvider).apiBaseUrl;
    final theme = Theme.of(context);
    return Scaffold(
      appBar: widget.embedded ? null : AppBar(title: const Text('Đăng nhập')),
      body: SafeArea(
        child: Center(
          child: SingleChildScrollView(
            padding: const EdgeInsets.all(24),
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 420),
              child: Form(
                key: _formKey,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Icon(Icons.eco, size: 56, color: theme.colorScheme.primary),
                    const SizedBox(height: 12),
                    Text(
                      'SAEMS',
                      textAlign: TextAlign.center,
                      style: theme.textTheme.headlineMedium,
                    ),
                    const SizedBox(height: 4),
                    Text(
                      'Đăng nhập server\n$server',
                      textAlign: TextAlign.center,
                      style: theme.textTheme.bodySmall,
                    ),
                    const SizedBox(height: 24),
                    TextFormField(
                      controller: _user,
                      enabled: !_busy,
                      autocorrect: false,
                      textInputAction: TextInputAction.next,
                      decoration: const InputDecoration(
                        labelText: 'Tên đăng nhập',
                        prefixIcon: Icon(Icons.person_outline),
                      ),
                      validator: (value) => (value ?? '').trim().isEmpty
                          ? 'Nhập tên đăng nhập'
                          : null,
                    ),
                    const SizedBox(height: 12),
                    TextFormField(
                      controller: _password,
                      enabled: !_busy,
                      obscureText: !_showPassword,
                      autocorrect: false,
                      enableSuggestions: false,
                      onFieldSubmitted: (_) => _busy ? null : _login(),
                      decoration: InputDecoration(
                        labelText: 'Mật khẩu',
                        prefixIcon: const Icon(Icons.lock_outline),
                        suffixIcon: IconButton(
                          tooltip: _showPassword ? 'Ẩn' : 'Hiện',
                          onPressed: () =>
                              setState(() => _showPassword = !_showPassword),
                          icon: Icon(
                            _showPassword
                                ? Icons.visibility_off
                                : Icons.visibility,
                          ),
                        ),
                      ),
                      validator: (value) =>
                          (value ?? '').isEmpty ? 'Nhập mật khẩu' : null,
                    ),
                    if (_error != null) ...[
                      const SizedBox(height: 12),
                      Text(
                        _error!,
                        style: TextStyle(color: theme.colorScheme.error),
                      ),
                    ],
                    const SizedBox(height: 20),
                    FilledButton.icon(
                      onPressed: _busy ? null : _login,
                      icon: _busy
                          ? const SizedBox.square(
                              dimension: 18,
                              child: CircularProgressIndicator(strokeWidth: 2),
                            )
                          : const Icon(Icons.login),
                      label: const Text('Đăng nhập'),
                    ),
                    const SizedBox(height: 8),
                    TextButton.icon(
                      onPressed: _busy
                          ? null
                          : () => Navigator.of(context).push<void>(
                              MaterialPageRoute(
                                builder: (_) => const SettingsScreen(),
                              ),
                            ),
                      icon: const Icon(Icons.settings_outlined),
                      label: const Text(
                        'Đổi server, dùng API token hoặc kết nối BLE / Wi-Fi AP',
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
