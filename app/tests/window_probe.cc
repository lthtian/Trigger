// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:50
// ABOUTME: An independently running resizable window for native integration tests.
#include <windows.h>
#include <shellapi.h>
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  int count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  WNDCLASSW type{};
  type.lpfnWndProc = WindowProcedure;
  type.hInstance = instance;
  type.lpszClassName = L"TriggerPlacementProbe";
  type.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  RegisterClassW(&type);
  HWND window = CreateWindowExW(
      0, type.lpszClassName, count > 1 ? arguments[1] : L"Trigger placement test",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 500, 300, nullptr, nullptr, instance, nullptr);
  if (arguments) LocalFree(arguments);
  if (!window) return 1;
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
