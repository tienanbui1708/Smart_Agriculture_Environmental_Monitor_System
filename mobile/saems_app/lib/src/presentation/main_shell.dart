import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../models/data_source.dart';
import '../state/app_state.dart';

import 'dashboard_screen.dart';
import 'device_screen.dart';
import 'history_screen.dart';
import 'login_screen.dart';
import 'nearby_screen.dart';
import 'settings_screen.dart';

class MainShell extends ConsumerStatefulWidget {
  const MainShell({super.key});

  @override
  ConsumerState<MainShell> createState() => _MainShellState();
}

class _MainShellState extends ConsumerState<MainShell> {
  int _index = 0;

  static const _screens = [
    DashboardScreen(),
    HistoryScreen(),
    NearbyScreen(),
    DeviceScreen(),
    SettingsScreen(),
  ];

  @override
  Widget build(BuildContext context) {
    // Server source with no stored credential: ask for a login first. A
    // legacy API token still counts as a credential, so existing setups keep
    // working. Loading/error states fall through to the normal shell.
    final source = ref.watch(currentSettingsProvider).source;
    final token = ref.watch(apiTokenProvider);
    if (source == DataSourceType.server &&
        token is AsyncData<String?> &&
        (token.value == null || token.value!.isEmpty)) {
      return const LoginScreen(embedded: true);
    }
    return Scaffold(
      body: SafeArea(
        child: IndexedStack(index: _index, children: _screens),
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _index,
        onDestinationSelected: (value) => setState(() => _index = value),
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.dashboard_outlined),
            label: 'Tổng quan',
          ),
          NavigationDestination(icon: Icon(Icons.show_chart), label: 'Lịch sử'),
          NavigationDestination(icon: Icon(Icons.sensors), label: 'Kết nối'),
          NavigationDestination(icon: Icon(Icons.memory), label: 'Thiết bị'),
          NavigationDestination(
            icon: Icon(Icons.settings_outlined),
            label: 'Cài đặt',
          ),
        ],
      ),
    );
  }
}
