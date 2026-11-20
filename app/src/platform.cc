// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:50
// ABOUTME: Windows application placement, file persistence, credentials and SMS transport.
#include "platform.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <set>

#include "desktops.h"
namespace trigger {
namespace {
using Microsoft::WRL::ComPtr;
std::string ReadBytes(const std::filesystem::path& path) {
  const auto size = std::filesystem::file_size(path);
  if (size > 2 * 1024 * 1024) throw std::runtime_error("配置文件超过 2 MB");
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("无法打开本地配置文件");
  std::string data(static_cast<size_t>(size), '\0');
  if (size && !stream.read(data.data(), static_cast<std::streamsize>(size)))
    throw std::runtime_error("读取配置文件失败");
  return data;
}
void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
  if (bytes.size() > 2 * 1024 * 1024) throw std::runtime_error("配置文件超过 2 MB");
  std::filesystem::create_directories(path.parent_path());
  const auto temporary =
      path.parent_path() / (path.filename().wstring() + L"." + Wide(Identifier()) + L".tmp");
  try {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("无法写入本地配置");
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) throw std::runtime_error("配置写入未完成");
    stream.close();
    CheckWin(MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
             "保存配置文件");
  } catch (...) {
    DeleteFileW(temporary.c_str());
    throw;
  }
}
void ValidateConnection(const Json& connection) {
  const auto url = connection.at("url").get<std::string>();
  const auto token = connection.at("token").get<std::string>();
  if (!IsWebUrl(url) || !url.starts_with("https://") ||
      url.find_first_of("?#") != std::string::npos)
    throw std::invalid_argument("服务器地址必须是 HTTPS，且不能带查询参数");
  if (token.empty() || token.size() > 4096 ||
      !std::all_of(token.begin(), token.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
    throw std::invalid_argument("读取令牌格式不正确");
}
struct HttpHandle {
  HINTERNET value = nullptr;
  explicit HttpHandle(HINTERNET handle) : value(handle) {
    CheckWin(handle != nullptr, "连接 HTTPS 服务");
  }
  ~HttpHandle() {
    if (value) WinHttpCloseHandle(value);
  }
  HttpHandle(const HttpHandle&) = delete;
  HttpHandle& operator=(const HttpHandle&) = delete;
};
Rect Rectangle(const RECT& rect) {
  return {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
}
Json Bounds(const Rect& rect) {
  return {{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}};
}
Rect Rectangle(const Json& rect) {
  return {rect.at("x"), rect.at("y"), rect.at("width"), rect.at("height")};
}
bool SamePath(const std::string& first, const std::string& second) {
  auto a = Wide(first), b = Wide(second);
  std::replace(a.begin(), a.end(), L'/', L'\\');
  std::replace(b.begin(), b.end(), L'/', L'\\');
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool BrowserPath(const std::string& path) {
  auto name = std::filesystem::path(Wide(path)).filename().wstring();
  std::transform(name.begin(), name.end(), name.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return name == L"chrome.exe" || name == L"msedge.exe";
}
const Json& ChooseMonitor(const Json& monitors, const std::string& id) {
  for (const auto& monitor : monitors)
    if (monitor["id"] == id) return monitor;
  for (const auto& monitor : monitors)
    if (monitor.at("primary").get<bool>()) return monitor;
  if (monitors.empty()) throw std::runtime_error("没有可用显示器");
  return monitors.front();
}
RECT VisibleBounds(HWND window) {
  RECT rect{};
  CheckWin(GetWindowRect(window, &rect), "读取窗口位置");
  RECT frame{};
  if (SUCCEEDED(
          DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
      frame.right > frame.left && frame.bottom > frame.top)
    return frame;
  return rect;
}
Json Describe(HWND window, const Json& monitors, Desktops* desktops) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (!pid || pid == GetCurrentProcessId() || !IsWindowVisible(window) ||
      GetWindow(window, GW_OWNER))
    return nullptr;
  if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return nullptr;
  const int length = std::min(2047, GetWindowTextLengthW(window));
  if (length < 1) return nullptr;
  std::wstring title(length + 1, L'\0');
  title.resize(GetWindowTextW(window, title.data(), length + 1));
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process) return nullptr;
  std::wstring path(32768, L'\0');
  DWORD path_length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &path_length)) return nullptr;
  path.resize(path_length);
  FILETIME start{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(process.get(), &start, &exit, &kernel, &user)) return nullptr;
  ULARGE_INTEGER started{};
  started.LowPart = start.dwLowDateTime;
  started.HighPart = start.dwHighDateTime;
  wchar_t window_class[256]{};
  GetClassNameW(window, window_class, 256);
  if (wcscmp(window_class, L"Progman") == 0 || wcscmp(window_class, L"WorkerW") == 0 ||
      wcscmp(window_class, L"Shell_TrayWnd") == 0)
    return nullptr;
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) return nullptr;
  std::string monitor_id = "primary";
  for (const auto& monitor : monitors)
    if (monitor["device"] == Utf8(info.szDevice)) {
      monitor_id = monitor["id"];
      break;
    }
  RECT rect = VisibleBounds(window);
  if (IsIconic(window)) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(window, &placement)) {
      rect = placement.rcNormalPosition;
      OffsetRect(&rect, info.rcWork.left - info.rcMonitor.left,
                 info.rcWork.top - info.rcMonitor.top);
    }
  }
  if (rect.right <= rect.left || rect.bottom <= rect.top) return nullptr;
  return {{"hwnd", reinterpret_cast<uint64_t>(window)},
          {"pid", pid},
          {"started", std::to_string(started.QuadPart)},
          {"path", Utf8(path)},
          {"title", Utf8(title)},
          {"class", Utf8(window_class)},
          {"rect", Bounds(Rectangle(rect))},
          {"monitor", monitor_id},
          {"desktop", desktops ? desktops->WindowDesktop(window) : 0},
          {"state", IsZoomed(window) ? "maximized" : "normal"}};
}
Json Inventory(bool desktop_details) {
  struct Context {
    Json windows = Json::array();
    Json monitors;
    Desktops* desktops = nullptr;
  } context;
  context.monitors = Monitors();
  std::unique_ptr<Desktops> desktops;
  if (desktop_details) {
    desktops = std::make_unique<Desktops>();
    context.desktops = desktops.get();
  }
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(parameter);
        try {
          auto value = Describe(window, context.monitors, context.desktops);
          if (!value.is_null()) context.windows.push_back(value);
        } catch (const std::exception&) { /* Windows can disappear while being enumerated. */
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));
  return context.windows;
}
Json LaunchRequest(const Json& entry) {
  return {{"kind", entry.at("kind")},
          {"path", entry.at("path")},
          {"args", entry.at("args")},
          {"workdir", entry.at("workdir")},
          {"url", entry.value("url", std::string())}};
}
Json Fingerprint(const Json& window, const Json& entry) {
  return {{"hwnd", window.at("hwnd")},
          {"pid", window.at("pid")},
          {"started", window.at("started")},
          {"path", window.at("path")},
          {"request", LaunchRequest(entry)}};
}
Json Match(const Json& entry, const Json& inventory, const Json& bindings,
           const std::set<uint64_t>& used) {
  if (!entry.at("reuse").get<bool>()) return nullptr;
  const auto id = entry.at("id").get<std::string>();
  if (bindings.contains(id) && bindings[id].value("request", Json()) != LaunchRequest(entry))
    return nullptr;
  for (const auto& window : inventory) {
    if (used.contains(window.at("hwnd").get<uint64_t>()) ||
        !SamePath(window.at("path"), entry.at("path")))
      continue;
    if (bindings.contains(id) && Fingerprint(window, entry) == bindings[id]) return window;
  }
  Json candidates = Json::array();
  const std::string title = entry.at("title");
  for (const auto& window : inventory) {
    if (used.contains(window.at("hwnd").get<uint64_t>()) ||
        !SamePath(window.at("path"), entry.at("path")))
      continue;
    if (!title.empty() && window.at("title") != title) continue;
    if (entry["kind"] == "web" && title.empty()) continue;
    candidates.push_back(window);
  }
  return candidates.size() == 1 ? candidates.front() : Json(nullptr);
}
Json Launch(const Json& entry, const Json& before, const std::atomic_bool* cancel) {
  const auto path = std::filesystem::path(Wide(entry.at("path")));
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("程序路径不存在：" + Utf8(path.wstring()));
  std::wstring arguments = Wide(entry.at("args"));
  if (entry["kind"] == "web") {
    if (!BrowserPath(entry.at("path"))) throw std::runtime_error("网页窗口请选择 Chrome 或 Edge");
    arguments = L"--new-window " + QuoteArgument(Wide(entry.at("url")));
  }
  const auto workdir = Wide(entry.at("workdir"));
  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  execute.lpVerb = L"open";
  execute.lpFile = path.c_str();
  execute.lpParameters = arguments.c_str();
  execute.lpDirectory = workdir.empty() ? nullptr : workdir.c_str();
  execute.nShow = SW_SHOWNORMAL;
  CheckWin(ShellExecuteExW(&execute), "启动应用");
  Handle process(execute.hProcess);
  std::set<uint64_t> existing;
  for (const auto& window : before) existing.insert(window.at("hwnd").get<uint64_t>());
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (cancel && cancel->load()) throw std::runtime_error("启动已取消");
    auto candidates = Json::array();
    for (const auto& window : Inventory(false))
      if (!existing.contains(window.at("hwnd").get<uint64_t>()) &&
          SamePath(window.at("path"), entry.at("path")))
        candidates.push_back(window);
    if (candidates.size() == 1) return candidates.front();
    if (candidates.size() > 1) {
      for (const auto& window : candidates)
        if (window.at("title") == entry.at("title")) return window;
      throw std::runtime_error("程序打开了多个窗口，无法唯一确定目标；请填写窗口标题");
    }
    Sleep(150);
  }
  throw std::runtime_error(
      "已发出启动请求，但 15 秒内未找到对应窗口；启动器或多进程应用可能需要指定实际 EXE");
}
Json Position(HWND window, const Json& entry, const Json& monitor) {
  const auto& bounds = entry.at("layout");
  const auto expected =
      Place({bounds.at("x"), bounds.at("y"), bounds.at("width"), bounds.at("height")},
            Rectangle(monitor.at("work")));
  if (IsIconic(window) || IsZoomed(window)) {
    ShowWindowAsync(window, SW_SHOWNOACTIVATE);
    for (int i = 0; i < 15 && (IsIconic(window) || IsZoomed(window)); ++i) Sleep(50);
  }
  RECT outer{};
  CheckWin(GetWindowRect(window, &outer), "读取窗口边框");
  const RECT frame = VisibleBounds(window);
  const int left = std::max(0L, frame.left - outer.left), top = std::max(0L, frame.top - outer.top);
  const int right = std::max(0L, outer.right - frame.right),
            bottom = std::max(0L, outer.bottom - frame.bottom);
  CheckWin(SetWindowPos(window, nullptr, expected.x - left, expected.y - top,
                        expected.width + left + right, expected.height + top + bottom,
                        SWP_NOACTIVATE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS),
           "设置窗口位置和大小");
  if (entry.at("state") == "maximized") {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    CheckWin(GetWindowPlacement(window, &placement), "读取窗口状态");
    placement.showCmd = SW_SHOWMAXIMIZED;
    CheckWin(SetWindowPlacement(window, &placement), "最大化窗口");
    for (int i = 0; i < 20 && !IsZoomed(window); ++i) Sleep(100);
    if (!IsZoomed(window)) throw std::runtime_error("应用没有接受最大化请求");
    return Bounds(Rectangle(VisibleBounds(window)));
  }
  Rect actual{};
  for (int i = 0; i < 25; ++i) {
    actual = Rectangle(VisibleBounds(window));
    if (std::abs(actual.x - expected.x) <= 2 && std::abs(actual.y - expected.y) <= 2 &&
        std::abs(actual.width - expected.width) <= 2 &&
        std::abs(actual.height - expected.height) <= 2)
      return Bounds(actual);
    Sleep(100);
  }
  throw std::runtime_error("应用限制了窗口尺寸或位置；实际 " + std::to_string(actual.width) + "×" +
                           std::to_string(actual.height));
}
}  // namespace

std::filesystem::path ExecutablePath() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) throw std::runtime_error("无法定位程序文件");
  path.resize(length);
  return path;
}
std::filesystem::path DataDirectory() {
  PWSTR path = nullptr;
  Check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path), "获取本地数据目录");
  std::filesystem::path directory(path);
  CoTaskMemFree(path);
  directory /= L"Trigger";
  std::filesystem::create_directories(directory);
  return directory;
}
Json ReadData(const std::filesystem::path& path) { return Json::parse(ReadBytes(path)); }
void WriteData(const std::filesystem::path& path, const Json& value) {
  WriteBytes(path, value.dump(2));
}
void SaveConnection(const std::filesystem::path& path, const Json& connection) {
  ValidateConnection(connection);
  auto bytes = connection.dump();
  DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())},
      output{};
  CheckWin(CryptProtectData(&input, L"Trigger SMS", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output),
           "加密连接凭据");
  const std::string encrypted(reinterpret_cast<char*>(output.pbData), output.cbData);
  LocalFree(output.pbData);
  SecureZeroMemory(bytes.data(), bytes.size());
  WriteBytes(path, encrypted);
}
Json LoadConnection(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return Json::object();
  auto bytes = ReadBytes(path);
  DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())},
      output{};
  CheckWin(CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                              &output),
           "解密当前账号的连接凭据");
  std::string plain(reinterpret_cast<char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  Json result;
  try {
    result = Json::parse(plain);
  } catch (...) {
    SecureZeroMemory(plain.data(), plain.size());
    throw;
  }
  SecureZeroMemory(plain.data(), plain.size());
  ValidateConnection(result);
  return result;
}
Json FetchEvents(const Json& connection) {
  ValidateConnection(connection);
  std::string base = connection.at("url");
  while (base.ends_with('/')) base.pop_back();
  const auto url = Wide(base + "/events");
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  CheckWin(WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts),
           "解析服务器地址");
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength),
      path(parts.lpszUrlPath, parts.dwUrlPathLength);
  thread_local HttpHandle session(WinHttpOpen(L"Trigger/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  CheckWin(WinHttpSetTimeouts(session.value, 3000, 3000, 3000, 5000), "设置网络超时");
  HttpHandle server(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
  HttpHandle request(WinHttpOpenRequest(server.value, L"GET", path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE));
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  CheckWin(
      WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)),
      "限制凭据重定向");
  const auto headers = L"Authorization: Bearer " + Wide(connection.at("token")) +
                       L"\r\nAccept: application/json\r\n";
  CheckWin(WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0),
           "发送验证码读取请求");
  CheckWin(WinHttpReceiveResponse(request.value, nullptr), "接收验证码服务响应");
  DWORD status = 0, size = sizeof(status);
  CheckWin(
      WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX),
      "读取 HTTP 状态");
  if (status != 200) throw std::runtime_error("验证码服务返回 HTTP " + std::to_string(status));
  std::string body;
  for (;;) {
    char buffer[4096];
    DWORD read = 0;
    CheckWin(WinHttpReadData(request.value, buffer, sizeof(buffer), &read), "读取验证码响应");
    if (!read) break;
    body.append(buffer, read);
    if (body.size() > 65536) throw std::runtime_error("验证码响应超过大小限制");
  }
  auto response = Json::parse(body);
  if (!response.at("events").is_array()) throw std::runtime_error("验证码响应格式不正确");
  return response.at("events");
}
Json Monitors() {
  struct Context {
    Json items = Json::array();
    std::exception_ptr error;
  } context;
  const BOOL result = EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(parameter);
        try {
          MONITORINFOEXW info{};
          info.cbSize = sizeof(info);
          CheckWin(GetMonitorInfoW(monitor, &info), "读取显示器信息");
          DISPLAY_DEVICEW device{};
          device.cb = sizeof(device);
          const bool identified = EnumDisplayDevicesW(info.szDevice, 0, &device,
                                                      EDD_GET_DEVICE_INTERFACE_NAME) != FALSE;
          UINT dpi_x = 96, dpi_y = 96;
          if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) dpi_x = 96;
          context.items.push_back(
              {{"id", Utf8(identified && device.DeviceID[0] ? device.DeviceID : info.szDevice)},
               {"device", Utf8(info.szDevice)},
               {"name",
                Utf8(identified && device.DeviceString[0] ? device.DeviceString : info.szDevice)},
               {"primary", (info.dwFlags & MONITORINFOF_PRIMARY) != 0},
               {"dpi", dpi_x},
               {"bounds", Bounds(Rectangle(info.rcMonitor))},
               {"work", Bounds(Rectangle(info.rcWork))}});
          return TRUE;
        } catch (...) {
          context.error = std::current_exception();
          return FALSE;
        }
      },
      reinterpret_cast<LPARAM>(&context));
  if (context.error) std::rethrow_exception(context.error);
  CheckWin(result, "枚举显示器");
  return context.items;
}
Json Browsers() {
  Json result = Json::array();
  for (const auto* name : {L"msedge.exe", L"chrome.exe"}) {
    const auto key =
        std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + name;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
      wchar_t path[32768]{};
      DWORD size = sizeof(path);
      if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) ==
              ERROR_SUCCESS &&
          std::filesystem::is_regular_file(path)) {
        result.push_back(
            {{"name", wcscmp(name, L"msedge.exe") == 0 ? "Microsoft Edge" : "Google Chrome"},
             {"path", Utf8(path)}});
        break;
      }
    }
  }
  return result;
}
Json Windows() { return Inventory(true); }
Json EntryFromWindow(const Json& window) {
  const auto monitors = Monitors();
  const auto& monitor = ChooseMonitor(monitors, window.at("monitor"));
  const auto layout = Capture(Rectangle(window.at("rect")), Rectangle(monitor.at("work")));
  auto name = window.at("title").get<std::string>();
  if (name.size() > 150)
    name = Utf8(std::filesystem::path(Wide(window.at("path"))).stem().wstring());
  return {{"id", Identifier()},
          {"name", name},
          {"kind", BrowserPath(window.at("path")) ? "web" : "app"},
          {"path", window.at("path")},
          {"url", ""},
          {"args", ""},
          {"workdir", ""},
          {"title", window.at("title")},
          {"monitor", monitor.at("id")},
          {"desktop", window.at("desktop")},
          {"state", window.at("state")},
          {"reuse", true},
          {"layout",
           {{"x", layout.x}, {"y", layout.y}, {"width", layout.width}, {"height", layout.height}}}};
}
Json Restore(const Json& scene, Json& bindings, const std::function<void(const Json&)>& progress,
             const std::atomic_bool* cancel) {
  ValidateScene(scene);
  const auto monitors = Monitors();
  Desktops desktops;
  Json items = Json::array();
  std::set<uint64_t> used;
  const int initial_desktop = desktops.Current();
  for (const auto& entry : scene.at("entries")) {
    Json item{{"id", entry.at("id")}, {"name", entry.at("name")}, {"ok", false}, {"reused", false}};
    try {
      if (cancel && cancel->load()) throw std::runtime_error("启动已取消");
      int target = entry.at("desktop");
      if (target == 0) target = initial_desktop;
      if (target)
        desktops.Ensure(target);
      else if (entry.at("desktop").get<int>() != 0)
        throw std::runtime_error(desktops.error());
      auto inventory = Inventory(false);
      Json window = Match(entry, inventory, bindings, used);
      item["reused"] = !window.is_null();
      if (window.is_null()) window = Launch(entry, inventory, cancel);
      const auto handle = window.at("hwnd").get<uint64_t>();
      used.insert(handle);
      bindings[entry.at("id").get<std::string>()] = Fingerprint(window, entry);
      const auto& monitor = ChooseMonitor(monitors, entry.at("monitor"));
      item["rect"] = Position(reinterpret_cast<HWND>(handle), entry, monitor);
      if (target) desktops.Move(reinterpret_cast<HWND>(handle), target);
      item["desktop"] = desktops.WindowDesktop(reinterpret_cast<HWND>(handle));
      item["monitor"] = monitor.at("id");
      item["hwnd"] = handle;
      item["ok"] = true;
      if (entry["monitor"] != "primary" && entry["monitor"] != monitor["id"])
        item["warning"] = "目标显示器未连接，已恢复到主显示器";
    } catch (const std::exception& error) {
      item["error"] = error.what();
    }
    items.push_back(item);
    if (progress) progress(item);
    if (cancel && cancel->load()) break;
  }
  const bool ok = items.size() == scene.at("entries").size() &&
                  std::all_of(items.begin(), items.end(),
                              [](const Json& item) { return item.at("ok").get<bool>(); });
  return {{"scene", scene.at("name")}, {"ok", ok}, {"items", items}};
}
std::filesystem::path CreateShortcut(const Json& scene, const std::filesystem::path& directory,
                                     const std::filesystem::path& data_directory) {
  ComPtr<IShellLinkW> link;
  Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)),
        "创建快捷方式");
  const auto executable = ExecutablePath();
  Check(link->SetPath(executable.c_str()), "设置快捷方式程序");
  const auto arguments = L"--scene " + QuoteArgument(Wide(scene.at("id"))) + L" --data-dir " +
                         QuoteArgument(std::filesystem::absolute(data_directory).wstring());
  Check(link->SetArguments(arguments.c_str()), "设置场景参数");
  Check(link->SetWorkingDirectory(executable.parent_path().c_str()), "设置工作目录");
  Check(link->SetIconLocation(executable.c_str(), 0), "设置图标");
  auto name = Wide(scene.at("name"));
  for (auto& character : name)
    if (wcschr(L"<>:\"/\\|?*", character) || character < 32) character = L'_';
  if (name.size() > 70) name.resize(70);
  name = L"Trigger - " + name;
  auto path = directory / (name + L".lnk");
  for (int i = 2; std::filesystem::exists(path); ++i)
    path = directory / (name + L" (" + std::to_wstring(i) + L").lnk");
  ComPtr<IPersistFile> persist;
  Check(link.As(&persist), "快捷方式存储");
  Check(persist->Save(path.c_str(), TRUE), "保存桌面快捷方式");
  return path;
}
std::wstring PickFile(HWND owner, bool folder) {
  ComPtr<IFileOpenDialog> dialog;
  Check(
      CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)),
      "选择文件");
  DWORD flags = 0;
  Check(dialog->GetOptions(&flags), "读取文件选择选项");
  Check(dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                           (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST)),
        "设置文件选择选项");
  if (!folder) {
    const COMDLG_FILTERSPEC filters[] = {{L"应用程序", L"*.exe"}};
    Check(dialog->SetFileTypes(1, filters), "设置文件类型");
  }
  const HRESULT shown = dialog->Show(owner);
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
  Check(shown, "显示文件选择窗口");
  ComPtr<IShellItem> item;
  Check(dialog->GetResult(&item), "获取选择结果");
  PWSTR path = nullptr;
  Check(item->GetDisplayName(SIGDN_FILESYSPATH, &path), "读取文件路径");
  const std::wstring value(path);
  CoTaskMemFree(path);
  return value;
}
void CopyText(HWND owner, const std::wstring& text) {
  const size_t size = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!memory) throw std::runtime_error("无法分配剪贴板内存");
  void* bytes = GlobalLock(memory);
  if (!bytes) {
    GlobalFree(memory);
    throw std::runtime_error("无法锁定剪贴板内存");
  }
  memcpy(bytes, text.c_str(), size);
  GlobalUnlock(memory);
  bool opened = false;
  for (int i = 0; i < 5 && !opened; ++i) {
    opened = OpenClipboard(owner) != FALSE;
    if (!opened) Sleep(30);
  }
  if (!opened) {
    GlobalFree(memory);
    throw std::runtime_error("剪贴板正在被其他程序使用，请重试");
  }
  const bool success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
  CloseClipboard();
  if (!success) {
    GlobalFree(memory);
    throw std::runtime_error("复制验证码失败");
  }
}
}  // namespace trigger
