// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:45
// ABOUTME: Creates one temporary desktop and verifies real window movement and cleanup.
#include <combaseapi.h>

#include <iostream>
#include <stdexcept>

#include "desktops.h"
using namespace trigger;
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return 2;
  int result = 0;
  {
    Desktops desktops;
    HWND window = nullptr;
    std::string created, original_id;
    int original = 0;
    try {
      Require(desktops.available(), desktops.error().c_str());
      const auto before = desktops.List();
      original = desktops.Current();
      Require(!before.empty() && original > 0, "desktop enumeration");
      original_id = before.at(original - 1).at("id");
      window = CreateWindowExW(0, L"STATIC", L"Trigger desktop verification",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 420, 220, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
      Require(window != nullptr, "test window creation");
      for (int i = 0; i < 30 && desktops.WindowDesktop(window) == 0; ++i) Sleep(100);
      Require(desktops.WindowDesktop(window) == original, "initial desktop membership");
      desktops.Ensure(static_cast<int>(before.size()) + 1);
      auto after = desktops.List();
      Require(after.size() == before.size() + 1, "temporary desktop creation");
      created = after.back().at("id");
      desktops.Move(window, static_cast<int>(after.size()));
      Require(desktops.WindowDesktop(window) == static_cast<int>(after.size()),
              "outward window movement");
      Require(desktops.Current() == original, "background move preserves active desktop");
      desktops.Move(window, original);
      Require(desktops.WindowDesktop(window) == original, "return window movement");
      desktops.Remove(created, original_id);
      created.clear();
      Require(desktops.List().size() == before.size(), "temporary desktop removed");
      std::cout << "PASS desktop enumeration, creation, real window movement, current-desktop "
                   "preservation and cleanup\n";
    } catch (const std::exception& error) {
      std::cout << "FAIL " << error.what() << '\n';
      result = 1;
    }
    if (window) {
      if (!created.empty()) {
        try {
          desktops.Move(window, original);
        } catch (...) {
          result = 1;
        }
      }
      DestroyWindow(window);
    }
    if (!created.empty()) {
      try {
        desktops.Remove(created, original_id);
      } catch (const std::exception& error) {
        std::cout << "Cleanup failed: " << error.what() << '\n';
        result = 1;
      }
    }
  }
  CoUninitialize();
  return result;
}
