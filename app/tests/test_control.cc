// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 22:22
// ABOUTME: Verifies the native test popup and cleans up only identified trial windows.
#include <ole2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <iostream>

#include "platform.h"
using namespace trigger;
using Microsoft::WRL::ComPtr;
namespace {
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
HWND Find(DWORD pid, const std::wstring& name) {
  struct Match {
    DWORD pid;
    std::wstring name;
    HWND window = nullptr;
    int count = 0;
  } match{pid, name};
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto& match = *reinterpret_cast<Match*>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        wchar_t name[256]{};
        GetClassNameW(window, name, 256);
        if (pid == match.pid && std::wstring(name).starts_with(match.name)) {
          match.window = window;
          ++match.count;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&match));
  return match.count == 1 ? match.window : nullptr;
}
void Screenshot(HWND window, const std::filesystem::path& path) {
  RECT rect{};
  Require(GetWindowRect(window, &rect) != 0, "popup dimensions");
  HDC screen = GetDC(window);
  Require(screen != nullptr, "popup DC");
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right - rect.left, rect.bottom - rect.top);
  ReleaseDC(window, screen);
  Require(memory && bitmap, "popup bitmap");
  const auto previous = SelectObject(memory, bitmap);
  const BOOL rendered = PrintWindow(window, memory, 0);
  SelectObject(memory, previous);
  DeleteDC(memory);
  try {
    Require(rendered != FALSE, "popup rendering");
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)),
          "image factory");
    ComPtr<IWICBitmap> source;
    Check(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source),
          "image bitmap");
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream), "image stream");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "image path");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG initialization");
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr), "PNG frame");
    Check(frame->Initialize(nullptr), "PNG frame initialization");
    Check(frame->SetSize(rect.right - rect.left, rect.bottom - rect.top), "PNG size");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
    Check(frame->SetPixelFormat(&format), "PNG format");
    Check(frame->WriteSource(source.Get(), nullptr), "PNG pixels");
    Check(frame->Commit(), "PNG frame save");
    Check(encoder->Commit(), "PNG save");
  } catch (...) {
    DeleteObject(bitmap);
    throw;
  }
  DeleteObject(bitmap);
}
struct Clipboard {
  ComPtr<IDataObject> saved;
  Clipboard() { Check(OleGetClipboard(&saved), "preserve clipboard"); }
  ~Clipboard() {
    if (saved) {
      OleSetClipboard(saved.Get());
      OleFlushClipboard();
    }
  }
};
}  // namespace
int wmain(int count, wchar_t** arguments) {
  if (FAILED(OleInitialize(nullptr))) return 2;
  int result = 0;
  try {
    Require(count >= 3, "test operation and argument required");
    const std::wstring operation = arguments[1];
    if (operation == L"popup") {
      Require(count == 5, "popup needs PID, screenshot and report paths");
      const DWORD pid = std::stoul(arguments[2]);
      HWND popup = nullptr;
      for (int i = 0; i < 100 && !popup; ++i) {
        popup = Find(pid, L"TriggerCodePopup");
        if (!popup) Sleep(100);
      }
      Require(popup != nullptr, "popup exists");
      Require((GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
              "popup does not activate");
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      Require(
          GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE,
          "primary monitor");
      RECT rect{};
      GetWindowRect(popup, &rect);
      Require(rect.left >= monitor.rcWork.left && rect.left - monitor.rcWork.left < 80,
              "popup lower-left x");
      Require(rect.bottom <= monitor.rcWork.bottom && monitor.rcWork.bottom - rect.bottom < 80,
              "popup lower-left y");
      Screenshot(popup, arguments[3]);
      const HWND foreground = GetForegroundWindow();
      {
        Clipboard clipboard;
        SendMessageW(popup, WM_LBUTTONUP, 0, MAKELPARAM(40, 65));
        Require(GetForegroundWindow() == foreground, "copy preserves foreground focus");
        Require(OpenClipboard(nullptr) != FALSE, "read clipboard result");
        const auto memory = GetClipboardData(CF_UNICODETEXT);
        const auto* value = memory ? static_cast<const wchar_t*>(GlobalLock(memory)) : nullptr;
        const bool copied = value && wcscmp(value, L"123456") == 0;
        if (value) GlobalUnlock(memory);
        CloseClipboard();
        Require(copied, "clicked popup copied synthetic code");
      }
      WriteData(arguments[4], {{"ok", true},
                               {"copy_verified", true},
                               {"focus_preserved", true},
                               {"clipboard_restored", true},
                               {"bounds",
                                {{"left", rect.left},
                                 {"top", rect.top},
                                 {"right", rect.right},
                                 {"bottom", rect.bottom}}}});
      std::cout << "PASS native popup placement, no activation, click-to-copy and clipboard "
                   "restoration\n";
    } else if (operation == L"https") {
      bool rejected = false;
      try {
        FetchEvents({{"url", Utf8(arguments[2])}, {"token", "trigger-invalid-transport-test"}});
      } catch (const std::exception& error) {
        rejected = std::string(error.what()) == "验证码服务返回 HTTP 401";
        if (!rejected) throw;
      }
      Require(rejected, "HTTPS endpoint rejects unauthenticated test request");
      std::cout << "PASS native HTTPS certificate verification and expected HTTP 401\n";
    } else if (operation == L"hotkey") {
      const DWORD pid = std::stoul(arguments[2]);
      Require(Find(pid, L"TriggerWorkspaceHost") != nullptr, "unique Trigger test host");
      for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
        Require((GetAsyncKeyState(key) & 0x8000) == 0, "test requires released modifier keys");
      INPUT inputs[6]{};
      const WORD keys[] = {VK_CONTROL, VK_MENU, '9', '9', VK_MENU, VK_CONTROL};
      for (int i = 0; i < 6; ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = keys[i];
        inputs[i].ki.dwFlags = i >= 3 ? KEYEVENTF_KEYUP : 0;
      }
      Require(SendInput(6, inputs, sizeof(INPUT)) == 6, "send registered test hotkey");
      std::cout << "Sent Ctrl+Alt+9 to registered test instance\n";
    } else if (operation == L"hidden") {
      const DWORD pid = std::stoul(arguments[2]);
      const auto host = Find(pid, L"TriggerWorkspaceHost");
      Require(host && !IsWindowVisible(host),
              "Trigger remains running with hidden management window");
      std::cout << "PASS native background host remains with hidden UI\n";
    } else if (operation == L"close") {
      Require(count == 4, "close needs creation report and binding file");
      const auto report = ReadData(arguments[2]), bindings = ReadData(arguments[3]);
      const auto windows = trigger::Windows();
      int closed = 0;
      for (const auto& item : report.at("items")) {
        if (!item.at("ok").get<bool>() || item.at("reused").get<bool>()) continue;
        const auto id = item.at("id").get<std::string>();
        Require(bindings.contains(id), "cleanup window binding");
        const auto& binding = bindings.at(id);
        for (const auto& window : windows)
          if (window["hwnd"] == binding["hwnd"] && window["pid"] == binding["pid"] &&
              window["started"] == binding["started"] && window["path"] == binding["path"]) {
            Require(PostMessageW(reinterpret_cast<HWND>(window.at("hwnd").get<uint64_t>()),
                                 WM_CLOSE, 0, 0) != FALSE,
                    "request owned test window close");
            ++closed;
          }
      }
      std::cout << "Requested normal closure of " << closed << " verified trial windows\n";
    } else if (operation == L"exit") {
      const DWORD pid = std::stoul(arguments[2]);
      const auto host = Find(pid, L"TriggerWorkspaceHost");
      Require(host != nullptr, "unique Trigger test host");
      Require(PostMessageW(host, WM_APP + 3, 0, 0) != FALSE, "request test application exit");
      Handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
      if (process)
        Require(WaitForSingleObject(process.get(), 15000) == WAIT_OBJECT_0, "normal test app exit");
      std::cout << "PASS normal test application shutdown\n";
    } else
      throw std::invalid_argument("unknown test operation");
  } catch (const std::exception& error) {
    std::cout << "FAIL " << error.what() << '\n';
    result = 1;
  }
  OleUninitialize();
  return result;
}
