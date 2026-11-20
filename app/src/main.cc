// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 22:04
// ABOUTME: WebView2 scene editor with native restoration, hotkeys and short-lived SMS popups.
#include "platform.h"

#include <WebView2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <windowsx.h>
#include <wrl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "desktops.h"

namespace trigger {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr UINT kAsync = WM_APP + 1, kTray = WM_APP + 2, kExit = WM_APP + 3, kShow = WM_APP + 4;
constexpr wchar_t kOrigin[] = L"https://trigger.local/index.html";
int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
struct Apartment {
  Apartment() { Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "初始化 Windows 组件"); }
  ~Apartment() { CoUninitialize(); }
};
class Application {
 public:
  Application(std::filesystem::path directory, std::wstring window_class)
      : directory_(std::move(directory)), window_class_(std::move(window_class)) {
    std::filesystem::create_directories(directory_);
    const auto browsers = Browsers();
    settings_ = std::filesystem::exists(directory_ / L"scenes.json")
                    ? ReadData(directory_ / L"scenes.json")
                    : DefaultSettings(browsers.empty() ? "msedge.exe"
                                                       : browsers[0].at("path").get<std::string>());
    ValidateSettings(settings_);
    if (!std::filesystem::exists(directory_ / L"scenes.json"))
      WriteData(directory_ / L"scenes.json", settings_);
    if (std::filesystem::exists(directory_ / L"windows.json")) {
      try {
        bindings_ = ReadData(directory_ / L"windows.json");
        if (!bindings_.is_object()) bindings_ = Json::object();
      } catch (const std::exception&) {
        bindings_ = Json::object();
      }
    }
    connection_ = LoadConnection(directory_ / L"connection.dat");
    otp_enabled_ = settings_.at("otp_enabled");
  }
  ~Application() {
    quitting_ = true;
    cancel_ = true;
    condition_.notify_all();
    if (restore_thread_.joinable()) restore_thread_.join();
    if (sms_thread_.joinable()) sms_thread_.join();
    MSG message{};
    while (PeekMessageW(&message, window_, kAsync, kAsync, PM_REMOVE))
      delete reinterpret_cast<Json*>(message.lParam);
    ReleaseEditor();
    if (popup_) DestroyWindow(popup_);
    if (tray_visible_) Shell_NotifyIconW(NIM_DELETE, &tray_);
    for (int i = 1; i <= 9; ++i) UnregisterHotKey(window_, i);
    if (window_) DestroyWindow(window_);
  }
  int Run(const std::string& scene_id, bool background) {
    WNDCLASSW type{};
    type.lpfnWndProc = Procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = window_class_.c_str();
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    CheckWin(RegisterClassW(&type) != 0, "注册应用窗口");
    const auto monitors = Monitors();
    auto monitor = monitors.front();
    for (const auto& item : monitors)
      if (item.at("primary").get<bool>()) monitor = item;
    const auto& work = monitor.at("work");
    const double scale = monitor.at("dpi").get<double>() / 96.0;
    const int width = std::min(work.at("width").get<int>() - 40, static_cast<int>(1160 * scale));
    const int height = std::min(work.at("height").get<int>() - 40, static_cast<int>(830 * scale));
    window_ =
        CreateWindowExW(0, window_class_.c_str(), L"Trigger · 场景启动器", WS_OVERLAPPEDWINDOW,
                        work.at("x").get<int>() + (work.at("width").get<int>() - width) / 2,
                        work.at("y").get<int>() + (work.at("height").get<int>() - height) / 2,
                        width, height, nullptr, nullptr, type.hInstance, this);
    CheckWin(window_ != nullptr, "创建应用窗口");
    RegisterHotkeys();
    UpdateTray();
    sms_thread_ = std::thread([this] { PollSms(); });
    if (!scene_id.empty())
      StartScene(scene_id);
    else if (!background)
      ShowEditor();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return 0;
  }

 private:
  static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      app->window_ = window;
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) {
      try {
        return app->Message(message, wparam, lparam);
      } catch (const std::exception& error) {
        app->Notice(error.what());
        return 0;
      }
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }
  LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SIZE) {
      ResizeEditor();
      return 0;
    }
    if (message == WM_DPICHANGED) {
      const auto* rect = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                   rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    if (message == WM_GETMINMAXINFO) {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      const UINT dpi = GetDpiForWindow(window_);
      limits->ptMinTrackSize = {MulDiv(850, dpi, 96), MulDiv(600, dpi, 96)};
      return 0;
    }
    if (message == WM_CLOSE) {
      HideEditor();
      return 0;
    }
    if (message == kExit) {
      Quit();
      return 0;
    }
    if (message == kShow) {
      ShowEditor();
      return 0;
    }
    if (message == WM_HOTKEY) {
      for (const auto& item : settings_.at("scenes"))
        if (item.at("hotkey").get<int>() == static_cast<int>(wparam)) {
          StartScene(item.at("id"));
          break;
        }
      return 0;
    }
    if (message == kTray) {
      if (lparam == WM_LBUTTONDBLCLK)
        ShowEditor();
      else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU)
        TrayMenu();
      return 0;
    }
    if (message == WM_COPYDATA) {
      auto* copy = reinterpret_cast<COPYDATASTRUCT*>(lparam);
      if (copy->cbData > 0 && copy->cbData < 4096) {
        auto request = Json::parse(static_cast<const char*>(copy->lpData),
                                   static_cast<const char*>(copy->lpData) + copy->cbData);
        const auto id = request.value("scene", std::string());
        if (id.empty())
          ShowEditor();
        else
          StartScene(id);
      }
      return TRUE;
    }
    if (message == kAsync) {
      std::unique_ptr<Json> event(reinterpret_cast<Json*>(lparam));
      Receive(*event);
      return 0;
    }
    if (message == WM_DISPLAYCHANGE) {
      if (web_ready_) Send({{"event", "state"}, {"data", State()}});
      return 0;
    }
    if (message == taskbar_message_) {
      tray_visible_ = false;
      UpdateTray();
      return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
  }
  void Send(const Json& message) {
    if (web_ && web_ready_)
      Check(web_->PostWebMessageAsJson(Wide(message.dump()).c_str()), "更新管理界面");
  }
  void Post(Json event) {
    if (quitting_) return;
    auto pointer = std::make_unique<Json>(std::move(event));
    if (PostMessageW(window_, kAsync, 0, reinterpret_cast<LPARAM>(pointer.get())))
      pointer.release();
  }
  Json State() {
    Desktops desktops;
    Json connection;
    {
      std::lock_guard lock(mutex_);
      connection = {{"configured", !connection_.empty()},
                    {"url", connection_.value("url", std::string())}};
    }
    Json result = {{"settings", settings_},
                   {"monitors", Monitors()},
                   {"browsers", Browsers()},
                   {"desktops", desktops.List()},
                   {"desktop_supported", desktops.available()},
                   {"desktop_error", desktops.error()},
                   {"connection", connection},
                   {"sms_status", sms_status_},
                   {"busy", busy_},
                   {"open_page", open_page_},
                   {"hotkey_errors", hotkey_errors_},
                   {"last_report", last_report_}};
    open_page_.clear();
    return result;
  }
  void ResizeEditor() {
    if (controller_) {
      RECT bounds{};
      GetClientRect(window_, &bounds);
      controller_->put_Bounds(bounds);
    }
  }
  void ShowEditor() {
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
    if (controller_ || loading_) return;
    loading_ = true;
    const auto generation = ++editor_generation_;
    const auto assets = ExecutablePath().parent_path() / L"web";
    if (!std::filesystem::exists(assets / L"index.html"))
      throw std::runtime_error("找不到 web/index.html，请完整解压试用包");
    const auto data = directory_ / L"WebView";
    Check(
        CreateCoreWebView2EnvironmentWithOptions(
            nullptr, data.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, generation, assets](HRESULT result,
                                           ICoreWebView2Environment* environment) -> HRESULT {
                  if (generation != editor_generation_ || quitting_) return S_OK;
                  try {
                    Check(result, "初始化 WebView2；请确认已安装 Microsoft Edge WebView2 Runtime");
                    environment_ = environment;
                    Check(
                        environment_->CreateCoreWebView2Controller(
                            window_,
                            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [this, generation, assets](
                                    HRESULT status,
                                    ICoreWebView2Controller* controller) -> HRESULT {
                                  if (generation != editor_generation_ || quitting_) {
                                    if (controller) controller->Close();
                                    return S_OK;
                                  }
                                  try {
                                    Check(status, "创建网页管理界面");
                                    controller_ = controller;
                                    Check(controller_->get_CoreWebView2(&web_), "读取网页视图");
                                    ComPtr<ICoreWebView2Settings> options;
                                    Check(web_->get_Settings(&options), "读取网页设置");
                                    options->put_AreDevToolsEnabled(FALSE);
                                    options->put_AreDefaultContextMenusEnabled(FALSE);
                                    options->put_IsStatusBarEnabled(FALSE);
                                    ComPtr<ICoreWebView2_3> mapping;
                                    Check(web_.As(&mapping), "WebView2 Runtime 版本过低");
                                    Check(mapping->SetVirtualHostNameToFolderMapping(
                                              L"trigger.local", assets.c_str(),
                                              COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS),
                                          "加载本地界面资源");
                                    EventRegistrationToken token{};
                                    Check(web_->add_NavigationStarting(
                                              Callback<ICoreWebView2NavigationStartingEventHandler>(
                                                  [](ICoreWebView2*,
                                                     ICoreWebView2NavigationStartingEventArgs* args)
                                                      -> HRESULT {
                                                    LPWSTR uri = nullptr;
                                                    const HRESULT result = args->get_Uri(&uri);
                                                    if (FAILED(result)) return result;
                                                    const bool allowed =
                                                        uri && wcscmp(uri, kOrigin) == 0;
                                                    CoTaskMemFree(uri);
                                                    if (!allowed) args->put_Cancel(TRUE);
                                                    return S_OK;
                                                  })
                                                  .Get(),
                                              &token),
                                          "限制管理界面导航");
                                    Check(
                                        web_->add_NewWindowRequested(
                                            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                                [](ICoreWebView2*,
                                                   ICoreWebView2NewWindowRequestedEventArgs* args)
                                                    -> HRESULT { return args->put_Handled(TRUE); })
                                                .Get(),
                                            &token),
                                        "限制网页弹窗");
                                    Check(
                                        web_->add_PermissionRequested(
                                            Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                                [](ICoreWebView2*,
                                                   ICoreWebView2PermissionRequestedEventArgs* args)
                                                    -> HRESULT {
                                                  return args->put_State(
                                                      COREWEBVIEW2_PERMISSION_STATE_DENY);
                                                })
                                                .Get(),
                                            &token),
                                        "限制网页权限");
                                    Check(web_->add_WebMessageReceived(
                                              Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                                  [this](ICoreWebView2*,
                                                         ICoreWebView2WebMessageReceivedEventArgs*
                                                             args) -> HRESULT {
                                                    try {
                                                      LPWSTR source = nullptr;
                                                      Check(args->get_Source(&source),
                                                            "读取界面来源");
                                                      const bool trusted =
                                                          source && wcscmp(source, kOrigin) == 0;
                                                      CoTaskMemFree(source);
                                                      if (!trusted) return S_OK;
                                                      LPWSTR text = nullptr;
                                                      Check(args->get_WebMessageAsJson(&text),
                                                            "读取界面请求");
                                                      std::wstring value(text ? text : L"");
                                                      CoTaskMemFree(text);
                                                      if (value.size() > 1024 * 1024)
                                                        throw std::runtime_error("界面请求过大");
                                                      Request(Json::parse(Utf8(value)));
                                                    } catch (const std::exception& error) {
                                                      Notice(error.what());
                                                    }
                                                    return S_OK;
                                                  })
                                                  .Get(),
                                              &token),
                                          "连接界面操作");
                                    ResizeEditor();
                                    Check(web_->Navigate(kOrigin), "打开本地管理界面");
                                    loading_ = false;
                                  } catch (const std::exception& error) {
                                    loading_ = false;
                                    MessageBoxW(window_, Wide(error.what()).c_str(), L"Trigger",
                                                MB_OK | MB_ICONERROR);
                                  }
                                  return S_OK;
                                })
                                .Get()),
                        "创建 WebView2 控件");
                  } catch (const std::exception& error) {
                    loading_ = false;
                    MessageBoxW(window_, Wide(error.what()).c_str(), L"Trigger",
                                MB_OK | MB_ICONERROR);
                  }
                  return S_OK;
                })
                .Get()),
        "启动 WebView2");
  }
  void ReleaseEditor() {
    ++editor_generation_;
    web_ready_ = false;
    loading_ = false;
    web_.Reset();
    if (controller_) controller_->Close();
    controller_.Reset();
    environment_.Reset();
  }
  void HideEditor() {
    ShowWindow(window_, SW_HIDE);
    ReleaseEditor();
    if (!Resident() && !busy_) Quit();
  }
  bool Resident() const {
    return settings_.at("otp_enabled").get<bool>() || settings_.at("hotkeys_enabled").get<bool>();
  }
  void Quit() {
    quitting_ = true;
    cancel_ = true;
    condition_.notify_all();
    ShowWindow(window_, SW_HIDE);
    PostQuitMessage(0);
  }
  void Request(const Json& request) {
    const auto id = request.at("id");
    Json answer;
    try {
      const std::string command = request.at("command");
      const Json payload = request.value("payload", Json::object());
      if (command == "ready") {
        web_ready_ = true;
        answer = State();
        Send({{"event", "codes"},
              {"data", {{"codes", inbox_.Recent(Now())}, {"status", sms_status_}}}});
      } else if (command == "save") {
        const auto settings = payload.at("settings");
        ValidateSettings(settings);
        WriteData(directory_ / L"scenes.json", settings);
        settings_ = settings;
        {
          std::lock_guard lock(mutex_);
          otp_enabled_ = settings_.at("otp_enabled");
          ++connection_generation_;
        }
        condition_.notify_all();
        RegisterHotkeys();
        UpdateTray();
        answer = State();
      } else if (command == "launch") {
        StartScene(payload.at("id"));
        answer = {{"started", true}};
      } else if (command == "capture") {
        answer = trigger::Windows();
        for (auto& item : answer) item["entry"] = EntryFromWindow(item);
      } else if (command == "pick_file" || command == "pick_folder")
        answer = Utf8(PickFile(window_, command == "pick_folder"));
      else if (command == "shortcut") {
        const auto& scene = FindScene(payload.at("id"));
        PWSTR desktop = nullptr;
        Check(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop), "查找桌面目录");
        const std::filesystem::path path(desktop);
        CoTaskMemFree(desktop);
        answer = Utf8(CreateShortcut(scene, path, directory_).wstring());
      } else if (command == "connection") {
        Json connection;
        {
          std::lock_guard lock(mutex_);
          connection = connection_;
        }
        connection["url"] = payload.at("url");
        if (!payload.at("token").get<std::string>().empty())
          connection["token"] = payload.at("token");
        SaveConnection(directory_ / L"connection.dat", connection);
        {
          std::lock_guard lock(mutex_);
          connection_ = connection;
          ++connection_generation_;
        }
        sms_initial_ = true;
        condition_.notify_all();
        answer = State();
      } else if (command == "test_popup") {
        Json code{{"id", "demo-" + Identifier()},
                  {"sender", "测试消息"},
                  {"code", "123456"},
                  {"receivedAt", Now()}};
        inbox_.Receive(Json::array({code}), Now());
        ShowCode(code);
        answer = true;
        Send({{"event", "codes"},
              {"data", {{"codes", inbox_.Recent(Now())}, {"status", sms_status_}}}});
      } else if (command == "copy") {
        bool found = false;
        for (const auto& code : inbox_.Recent(Now()))
          if (code.at("id") == payload.at("id")) {
            CopyText(window_, Wide(code.at("code")));
            found = true;
            break;
          }
        if (!found) throw std::runtime_error("验证码已经过期");
        answer = true;
      } else if (command == "exit") {
        answer = true;
        PostMessageW(window_, kExit, 0, 0);
      } else
        throw std::invalid_argument("未知操作");
      Send({{"reply", id}, {"data", answer}});
    } catch (const std::exception& error) {
      Send({{"reply", id}, {"error", error.what()}});
    }
  }
  const Json& FindScene(const std::string& id) const {
    for (const auto& scene : settings_.at("scenes"))
      if (scene.at("id") == id) return scene;
    throw std::runtime_error("没有找到指定场景，请重新创建快捷方式");
  }
  void StartScene(const std::string& id) {
    if (busy_) throw std::runtime_error("另一个场景正在启动，请等待完成");
    const Json scene = FindScene(id);
    ValidateScene(scene);
    if (restore_thread_.joinable()) restore_thread_.join();
    busy_ = true;
    cancel_ = false;
    Json bindings = bindings_;
    restore_thread_ = std::thread([this, scene, bindings]() mutable {
      try {
        Apartment apartment;
        auto result = Restore(
            scene, bindings,
            [this](const Json& item) { Post({{"event", "progress"}, {"data", item}}); }, &cancel_);
        Post({{"event", "complete"}, {"data", result}, {"bindings", bindings}});
      } catch (const std::exception& error) {
        Post({{"event", "complete"},
              {"data",
               {{"ok", false},
                {"scene", scene.at("name")},
                {"items",
                 Json::array(
                     {{{"name", scene.at("name")}, {"ok", false}, {"error", error.what()}}})}}},
              {"bindings", bindings}});
      }
    });
  }
  void RegisterHotkeys() {
    for (int i = 1; i <= 9; ++i) UnregisterHotKey(window_, i);
    hotkey_errors_ = Json::array();
    if (!settings_.at("hotkeys_enabled").get<bool>()) return;
    for (const auto& scene : settings_.at("scenes")) {
      const int key = scene.at("hotkey");
      if (key && !RegisterHotKey(window_, key, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, '0' + key))
        hotkey_errors_.push_back("Ctrl + Alt + " + std::to_string(key) + " 已被其他程序占用");
    }
  }
  void UpdateTray() {
    if (!Resident()) {
      if (tray_visible_) Shell_NotifyIconW(NIM_DELETE, &tray_);
      tray_visible_ = false;
      return;
    }
    if (tray_visible_) return;
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = window_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = kTray;
    tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(tray_.szTip, 128, L"Trigger · 双击打开场景");
    tray_visible_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
  }
  void Notice(const std::string& text) noexcept {
    try {
      if (web_ready_) {
        try {
          Send({{"event", "notice"}, {"data", text}});
          return;
        } catch (const std::exception&) {
        }
      }
      if (tray_visible_) {
        tray_.uFlags = NIF_INFO;
        tray_.dwInfoFlags = NIIF_WARNING;
        StringCchCopyW(tray_.szInfoTitle, 64, L"Trigger");
        StringCchCopyW(tray_.szInfo, 256, Wide(text).substr(0, 240).c_str());
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
      } else
        MessageBoxW(window_, Wide(text).c_str(), L"Trigger", MB_OK | MB_ICONERROR);
    } catch (...) {
      OutputDebugStringW(L"Trigger: unable to display error notification\n");
    }
  }
  void TrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"打开 Trigger");
    AppendMenuW(menu, MF_STRING, 2, L"最近验证码");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    size_t index = 0;
    for (const auto& scene : settings_.at("scenes"))
      AppendMenuW(menu, MF_STRING, 100 + index++, Wide(scene.at("name")).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"退出后台");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y,
                                        0, window_, nullptr);
    DestroyMenu(menu);
    if (command == 1) {
      open_page_ = "scenes";
      ShowEditor();
    } else if (command == 2) {
      open_page_ = "codes";
      ShowEditor();
      if (web_ready_) Send({{"event", "state"}, {"data", State()}});
    } else if (command == 3)
      Quit();
    else if (command >= 100 && command < 100 + settings_["scenes"].size())
      StartScene(settings_["scenes"][command - 100]["id"]);
  }
  void PollSms() {
    int failures = 0;
    while (!quitting_) {
      Json connection;
      bool enabled = false;
      unsigned generation = 0;
      {
        std::lock_guard lock(mutex_);
        connection = connection_;
        enabled = otp_enabled_;
        generation = connection_generation_;
      }
      int wait_seconds = 3;
      if (enabled && !connection.empty()) {
        try {
          auto events = FetchEvents(connection);
          Post({{"event", "sms"}, {"data", events}, {"status", "已连接 · 每 3 秒检查"}});
          failures = 0;
        } catch (const std::exception& error) {
          wait_seconds = std::min(30, 3 * (1 << std::min(3, failures++)));
          Post({{"event", "sms"}, {"status", std::string("连接失败：") + error.what()}});
        }
      } else {
        Post({{"event", "sms"}, {"status", enabled ? "尚未配置验证码连接" : "验证码提醒已关闭"}});
        wait_seconds = 30;
      }
      std::unique_lock lock(mutex_);
      condition_.wait_for(lock, std::chrono::seconds(wait_seconds),
                          [&] { return quitting_ || generation != connection_generation_; });
    }
  }
  void Receive(const Json& event) {
    const std::string type = event.at("event");
    if (type == "sms") {
      sms_status_ = event.at("status");
      if (event.contains("data")) {
        const auto fresh = inbox_.Receive(event.at("data"), Now());
        if (!sms_initial_ && !fresh.empty() && settings_.at("otp_enabled").get<bool>())
          ShowCode(fresh.front());
        sms_initial_ = false;
      }
      Send({{"event", "codes"},
            {"data", {{"codes", inbox_.Recent(Now())}, {"status", sms_status_}}}});
    } else if (type == "complete") {
      busy_ = false;
      bindings_ = event.at("bindings");
      WriteData(directory_ / L"windows.json", bindings_);
      last_report_ = event.at("data");
      WriteData(directory_ / L"last-restore.json", last_report_);
      Send({{"event", "complete"}, {"data", last_report_}});
      if (last_report_.at("ok").get<bool>() && settings_.at("hide_after_launch").get<bool>())
        HideEditor();
      else if (!last_report_.at("ok").get<bool>()) {
        Notice("部分窗口未能恢复，打开 Trigger 查看启动结果");
        ShowEditor();
      } else if (!Resident() && !IsWindowVisible(window_))
        Quit();
    } else
      Send(event);
  }
  static LRESULT CALLBACK PopupProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_TIMER) {
      DestroyWindow(window);
      return 0;
    }
    if (message == WM_DESTROY) {
      app->popup_ = nullptr;
      return 0;
    }
    if (message == WM_PAINT) {
      app->PaintCode(window);
      return 0;
    }
    if (message == WM_LBUTTONUP) {
      RECT rect{};
      GetClientRect(window, &rect);
      const int scale = GetDpiForWindow(window);
      if (GET_X_LPARAM(lparam) > rect.right - MulDiv(40, scale, 96) &&
          GET_Y_LPARAM(lparam) < MulDiv(40, scale, 96)) {
        DestroyWindow(window);
        return 0;
      }
      try {
        if (Now() - app->popup_code_.at("receivedAt").get<double>() > 300)
          throw std::runtime_error("验证码已过期");
        CopyText(app->window_, Wide(app->popup_code_.at("code")));
        app->popup_copied_ = true;
        InvalidateRect(window, nullptr, TRUE);
        SetTimer(window, 1, 1200, nullptr);
      } catch (const std::exception& error) {
        app->Notice(error.what());
      }
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }
  void ShowCode(const Json& code) {
    popup_code_ = code;
    popup_copied_ = false;
    if (!popup_) {
      WNDCLASSW type{};
      type.lpfnWndProc = PopupProcedure;
      type.hInstance = GetModuleHandleW(nullptr);
      type.lpszClassName = L"TriggerCodePopup";
      type.hCursor = LoadCursorW(nullptr, IDC_HAND);
      RegisterClassW(&type);
      const auto monitors = Monitors();
      auto monitor = monitors.front();
      for (const auto& item : monitors)
        if (item.at("primary").get<bool>()) monitor = item;
      const int dpi = monitor.at("dpi");
      const auto& work = monitor.at("work");
      const int width = MulDiv(330, dpi, 96), height = MulDiv(135, dpi, 96),
                gap = MulDiv(18, dpi, 96);
      popup_ =
          CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName,
                          L"Trigger 验证码", WS_POPUP | WS_BORDER, work.at("x").get<int>() + gap,
                          work.at("y").get<int>() + work.at("height").get<int>() - height - gap,
                          width, height, nullptr, nullptr, type.hInstance, this);
      CheckWin(popup_ != nullptr, "显示验证码弹窗");
    }
    InvalidateRect(popup_, nullptr, TRUE);
    ShowWindow(popup_, SW_SHOWNOACTIVATE);
    SetTimer(popup_, 1, 15000, nullptr);
  }
  void PaintCode(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT area{};
    GetClientRect(window, &area);
    HBRUSH background = CreateSolidBrush(RGB(245, 250, 245));
    FillRect(dc, &area, background);
    DeleteObject(background);
    const UINT dpi = GetDpiForWindow(window);
    auto pixel = [dpi](int n) { return MulDiv(n, dpi, 96); };
    SetBkMode(dc, TRANSPARENT);
    auto line = [&](const std::wstring& text, int size, int weight, COLORREF color, RECT rect) {
      HFONT font = CreateFontW(-pixel(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Microsoft YaHei UI");
      const auto previous = SelectObject(dc, font);
      SetTextColor(dc, color);
      DrawTextW(dc, text.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      SelectObject(dc, previous);
      DeleteObject(font);
    };
    line(L"Trigger · " + Wide(popup_code_.value("sender", std::string("验证码"))), 11, FW_NORMAL,
         RGB(91, 121, 102), {pixel(20), pixel(12), area.right - pixel(42), pixel(36)});
    line(L"×", 19, FW_NORMAL, RGB(117, 139, 121),
         {area.right - pixel(30), pixel(6), area.right - pixel(8), pixel(36)});
    line(popup_copied_ ? L"已复制" : Wide(popup_code_.at("code")), 30, FW_SEMIBOLD, RGB(26, 77, 54),
         {pixel(20), pixel(40), area.right - pixel(20), pixel(85)});
    line(popup_copied_ ? L"可以粘贴到验证码输入框"
                       : L"点击复制验证码                         15 秒后收起",
         11, FW_NORMAL, RGB(112, 132, 113),
         {pixel(20), pixel(94), area.right - pixel(12), pixel(120)});
    EndPaint(window, &paint);
  }
  std::filesystem::path directory_;
  std::wstring window_class_;
  HWND window_ = nullptr, popup_ = nullptr;
  Json settings_, bindings_ = Json::object(), connection_ = Json::object(),
                  hotkey_errors_ = Json::array(), last_report_, popup_code_;
  CodeInbox inbox_;
  ComPtr<ICoreWebView2Environment> environment_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> web_;
  NOTIFYICONDATAW tray_{};
  bool tray_visible_ = false, loading_ = false, web_ready_ = false, busy_ = false,
       sms_initial_ = true, popup_copied_ = false;
  unsigned editor_generation_ = 0, connection_generation_ = 0;
  UINT taskbar_message_ = RegisterWindowMessageW(L"TaskbarCreated");
  std::string sms_status_ = "正在检查连接", open_page_ = "scenes";
  std::atomic_bool quitting_{false}, cancel_{false};
  std::thread sms_thread_, restore_thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool otp_enabled_ = false;
};
}  // namespace
int RunApplication() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Apartment apartment;
  int count = 0;
  LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!values) throw std::runtime_error("无法读取启动参数");
  std::vector<std::wstring> arguments(values, values + count);
  LocalFree(values);
  auto argument = [&](const std::wstring& name) -> std::wstring {
    for (size_t i = 1; i + 1 < arguments.size(); ++i)
      if (arguments[i] == name) return arguments[i + 1];
    return {};
  };
  const auto custom = argument(L"--data-dir");
  const auto directory = custom.empty() ? DataDirectory() : std::filesystem::path(custom);
  std::filesystem::create_directories(directory);
  const auto import = argument(L"--import-sms");
  if (!import.empty()) {
    auto settings = ReadData(import);
    const auto server = argument(L"--server");
    if (server.empty()) throw std::runtime_error("导入连接需要 --server HTTPS地址");
    SaveConnection(directory / L"connection.dat",
                   {{"url", Utf8(server)}, {"token", settings.at("readToken")}});
    return 0;
  }
  const auto diagnose = argument(L"--diagnose");
  if (!diagnose.empty()) {
    Desktops desktops;
    WriteData(diagnose, {{"monitors", Monitors()},
                         {"browsers", Browsers()},
                         {"desktops", desktops.List()},
                         {"desktop_supported", desktops.available()},
                         {"desktop_error", desktops.error()}});
    return 0;
  }
  const auto check = argument(L"--check-sms");
  if (!check.empty()) {
    try {
      const auto events = FetchEvents(LoadConnection(directory / L"connection.dat"));
      WriteData(check, {{"ok", true}, {"event_count", events.size()}});
      return 0;
    } catch (const std::exception& error) {
      WriteData(check, {{"ok", false}, {"error", error.what()}});
      return 1;
    }
  }
  const auto class_name =
      L"TriggerWorkspaceHost" +
      std::to_wstring(std::hash<std::wstring>{}(std::filesystem::absolute(directory).wstring()));
  const auto mutex_name = L"Local\\" + class_name;
  Handle single(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
  const DWORD mutex_error = GetLastError();
  CheckWin(static_cast<bool>(single), "创建运行实例");
  const auto scene = Utf8(argument(L"--scene"));
  if (mutex_error == ERROR_ALREADY_EXISTS) {
    HWND window = nullptr;
    for (int i = 0; i < 20 && !window; ++i) {
      window = FindWindowW(class_name.c_str(), nullptr);
      if (!window) Sleep(100);
    }
    if (!window) throw std::runtime_error("Trigger 正在启动，请稍后再试");
    const auto command = Json({{"scene", scene}}).dump();
    COPYDATASTRUCT data{};
    data.cbData = static_cast<DWORD>(command.size());
    data.lpData = const_cast<char*>(command.data());
    DWORD_PTR result = 0;
    CheckWin(SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                 SMTO_ABORTIFHUNG, 5000, &result) != 0,
             "通知已运行的 Trigger");
    return 0;
  }
  Application app(directory, class_name);
  return app.Run(scene,
                 std::find(arguments.begin(), arguments.end(), L"--background") != arguments.end());
}
}  // namespace trigger
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  try {
    return trigger::RunApplication();
  } catch (const std::exception& error) {
    MessageBoxW(nullptr, trigger::Wide(error.what()).c_str(), L"Trigger 启动失败",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
