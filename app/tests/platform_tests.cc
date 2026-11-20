// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:50
// ABOUTME: Uses real files, DPAPI, shortcuts and a separate test process to validate restoration.
#include <shobjidl.h>
#include <wrl/client.h>

#include <fstream>
#include <iostream>

#include "desktops.h"
#include "platform.h"
using namespace trigger;
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
  const auto directory =
      std::filesystem::temp_directory_path() / Wide("Trigger-test-" + Identifier());
  std::filesystem::create_directories(directory);
  int failures = 0;
  auto test = [&](const char* name, const std::function<void()>& action) {
    try {
      action();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cout << "FAIL " << name << ": " << error.what() << '\n';
    }
  };
  test("UTF-8 and atomic settings storage", [&] {
    Json settings = DefaultSettings("C:/程序/browser.exe");
    WriteData(directory / L"配置.json", settings);
    Require(ReadData(directory / L"配置.json") == settings, "settings round trip");
    settings["scenes"][0]["name"] = "开发场景";
    WriteData(directory / L"配置.json", settings);
    Require(ReadData(directory / L"配置.json") == settings, "atomic replacement");
  });
  test("DPAPI credential persistence", [&] {
    Json connection{{"url", "https://example.com"}, {"token", "synthetic-test-token-012345"}};
    SaveConnection(directory / L"connection.dat", connection);
    Require(LoadConnection(directory / L"connection.dat") == connection, "credential round trip");
    std::ifstream file(directory / L"connection.dat", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    Require(bytes.find("synthetic-test-token") == std::string::npos, "token encrypted on disk");
  });
  test("actual monitor work areas", [&] {
    const auto monitors = Monitors();
    Require(!monitors.empty(), "monitor enumeration");
    for (const auto& monitor : monitors)
      Require(monitor["work"]["width"].get<int>() > 0, "positive work area");
  });
  test("desktop shortcut resolves executable and scene", [&] {
    auto scene = DefaultSettings("browser.exe")["scenes"][0];
    auto path = CreateShortcut(scene, directory, directory);
    Require(std::filesystem::exists(path), "shortcut created");
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)),
          "shortcut instance");
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    Check(link.As(&persist), "shortcut persistence");
    Check(persist->Load(path.c_str(), STGM_READ), "load shortcut");
    wchar_t arguments[1024]{};
    Check(link->GetArguments(arguments, 1024), "read arguments");
    Require(std::wstring(arguments).find(Wide(scene["id"])) != std::wstring::npos,
            "scene identifier present");
    Require(std::wstring(arguments).find(directory.wstring()) != std::wstring::npos,
            "configuration directory present");
  });
  const auto title = "Trigger verification " + Identifier();
  HWND probe = nullptr;
  test("launch, place, reuse and cross-desktop restore", [&] {
    auto scene = DefaultSettings("browser.exe")["scenes"][0];
    scene["entries"].erase(1);
    auto& entry = scene["entries"][0];
    entry["kind"] = "app";
    entry["name"] = "验证窗口";
    const auto path = ExecutablePath().parent_path() / L"Trigger.WindowProbe.exe";
    entry["path"] = Utf8(path.wstring());
    entry["args"] = Utf8(QuoteArgument(Wide(title)));
    entry["title"] = title;
    entry["layout"] = {{"x", 0}, {"y", 0}, {"width", .5}, {"height", .5}};
    Json bindings = Json::object();
    auto first = Restore(scene, bindings);
    probe = FindWindowW(L"TriggerPlacementProbe", Wide(title).c_str());
    Require(first.at("ok").get<bool>() && probe, "launch and placement succeeded");
    auto second = Restore(scene, bindings);
    Require(second.at("ok").get<bool>(), "second restoration succeeded");
    Require(second["items"][0]["reused"].get<bool>(), "window reused");
    entry["monitor"] = "disconnected-test-display";
    auto fallback = Restore(scene, bindings);
    Require(fallback.at("ok").get<bool>() && fallback["items"][0].contains("warning"),
            "missing monitor falls back with a warning");
    entry["monitor"] = "primary";
    entry["state"] = "maximized";
    Require(Restore(scene, bindings).at("ok").get<bool>() && IsZoomed(probe),
            "maximize restored window");
    entry["state"] = "normal";
    Require(Restore(scene, bindings).at("ok").get<bool>() && !IsZoomed(probe),
            "restore normal window size");
    for (const auto& monitor : Monitors()) {
      entry["monitor"] = monitor.at("id");
      Require(Restore(scene, bindings).at("ok").get<bool>(), "placement on each connected monitor");
    }
    entry["monitor"] = "primary";
    Require(Restore(scene, bindings).at("ok").get<bool>(), "return to primary monitor");
    int found = 0;
    Json captured;
    for (const auto& window : trigger::Windows())
      if (window["title"] == title) {
        ++found;
        captured = EntryFromWindow(window);
      }
    Require(found == 1, "one window across repeated runs");
    Require(captured["path"] == entry["path"], "capture executable");
    auto capture_scene = scene;
    capture_scene["entries"][0] = captured;
    ValidateScene(capture_scene);
    const auto changed_title = title + " arguments";
    entry["args"] = Utf8(QuoteArgument(Wide(changed_title)));
    entry["title"] = changed_title;
    auto changed = Restore(scene, bindings);
    const auto changed_probe = FindWindowW(L"TriggerPlacementProbe", Wide(changed_title).c_str());
    const bool changed_ok =
        changed.at("ok").get<bool>() && !changed["items"][0]["reused"].get<bool>() && changed_probe;
    if (changed_probe) PostMessageW(changed_probe, WM_CLOSE, 0, 0);
    Require(changed_ok, "changed launch arguments open the requested window");
    entry["args"] = Utf8(QuoteArgument(Wide(title)));
    entry["title"] = title;
    bindings = Json::object();
    Desktops desktops;
    const auto original = desktops.Current();
    auto before = desktops.List();
    std::string temporary;
    try {
      desktops.Ensure(static_cast<int>(before.size()) + 1);
      temporary = desktops.List().back().at("id");
      entry["desktop"] = before.size() + 1;
      auto moved = Restore(scene, bindings);
      Require(moved["ok"].get<bool>(), "cross-process desktop restore");
      Require(desktops.WindowDesktop(probe) == static_cast<int>(before.size()) + 1,
              "cross-process desktop verified");
      desktops.Move(probe, original);
      desktops.Remove(temporary, before[original - 1]["id"]);
      temporary.clear();
    } catch (...) {
      if (!temporary.empty()) {
        desktops.Move(probe, original);
        desktops.Remove(temporary, before[original - 1]["id"]);
      }
      throw;
    }
  });
  if (!probe) probe = FindWindowW(L"TriggerPlacementProbe", Wide(title).c_str());
  if (probe) PostMessageW(probe, WM_CLOSE, 0, 0);
  std::filesystem::remove_all(directory);
  CoUninitialize();
  std::cout << (failures ? "FAILED" : "PASSED") << " native integration tests\n";
  return failures ? 1 : 0;
}
