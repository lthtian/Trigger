// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:45
// ABOUTME: Implements desktop discovery and placement through Windows Shell interfaces.
#include "desktops.h"

#include <servprov.h>
#include <shobjidl.h>
#include <winternl.h>
#include <wrl/client.h>

#include "win.h"
namespace trigger {
namespace {
using Microsoft::WRL::ComPtr;
// Shell interface declarations follow MScholtes/VirtualDesktop (MIT), Windows 10 edition.
// The ABI is enabled only for Windows builds verified by this implementation.
struct __declspec(uuid("372E1D3B-38D3-42E4-A15B-8AB2B178F513")) ApplicationView : IUnknown {};
struct __declspec(uuid("1841C6D7-4F9D-42C0-AF41-8747538F10E5")) ApplicationViews : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, IObjectArray**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, ApplicationView**) = 0;
};
struct __declspec(uuid("FF72FFDD-BE7E-43FC-9C03-AD81681E88E4")) ShellDesktop : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE IsViewVisible(ApplicationView*, BOOL*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetId(GUID*) = 0;
};
struct __declspec(uuid("F31574D6-B682-4CDC-BD56-1827860ABEC6")) ShellDesktops : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(ApplicationView*, ShellDesktop*) = 0;
  virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(ApplicationView*, BOOL*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(ShellDesktop**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(ShellDesktop*, int, ShellDesktop**) = 0;
  virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(ShellDesktop*) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateDesktop(ShellDesktop**) = 0;
  virtual HRESULT STDMETHODCALLTYPE RemoveDesktop(ShellDesktop*, ShellDesktop*) = 0;
  virtual HRESULT STDMETHODCALLTYPE FindDesktop(const GUID*, ShellDesktop**) = 0;
};
constexpr GUID kShell = {
    0xc2f03a33, 0x21f5, 0x47fa, {0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39}};
constexpr GUID kDesktopService = {
    0xc5e0cdca, 0x7b6e, 0x41b2, {0x9f, 0xc4, 0xd9, 0x39, 0x75, 0xcc, 0x46, 0x7b}};
}  // namespace
struct Desktops::State {
  ComPtr<IServiceProvider> shell;
  ComPtr<ShellDesktops> desktops;
  ComPtr<ApplicationViews> views;
  ComPtr<IVirtualDesktopManager> public_manager;
  std::string error;
  ComPtr<ShellDesktop> At(int number) const {
    if (!desktops) throw std::runtime_error(error);
    ComPtr<IObjectArray> array;
    Check(desktops->GetDesktops(&array), "读取虚拟桌面");
    UINT count = 0;
    Check(array->GetCount(&count), "读取桌面数量");
    if (number < 1 || static_cast<UINT>(number) > count)
      throw std::invalid_argument("目标桌面不存在");
    ComPtr<ShellDesktop> desktop;
    Check(array->GetAt(number - 1, IID_PPV_ARGS(&desktop)), "读取目标桌面");
    return desktop;
  }
};
Desktops::Desktops() : state_(std::make_unique<State>()) {
  try {
    Check(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(&state_->public_manager)),
          "连接 Windows 桌面服务");
    using GetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto get_version = reinterpret_cast<GetVersion>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!get_version || get_version(&version) != 0 || version.dwBuildNumber != 19045)
      throw std::runtime_error("跨虚拟桌面功能目前适配 Windows 10 22H2（19045）；本机版本尚未验证");
    Check(CoCreateInstance(kShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&state_->shell)),
          "连接 Windows Shell");
    Check(state_->shell->QueryService(kDesktopService, IID_PPV_ARGS(&state_->desktops)),
          "连接虚拟桌面管理");
    Check(state_->shell->QueryService(__uuidof(ApplicationViews), IID_PPV_ARGS(&state_->views)),
          "连接窗口桌面服务");
  } catch (const std::exception& error) {
    state_->error = error.what();
    state_->desktops.Reset();
    state_->views.Reset();
  }
}
Desktops::~Desktops() = default;
bool Desktops::available() const { return state_->desktops && state_->views; }
std::string Desktops::error() const { return state_->error; }
Json Desktops::List() const {
  Json result = Json::array();
  if (!available()) return result;
  ComPtr<IObjectArray> array;
  Check(state_->desktops->GetDesktops(&array), "枚举虚拟桌面");
  UINT count = 0;
  Check(array->GetCount(&count), "读取桌面数量");
  if (count > 256) throw std::runtime_error("桌面数量异常");
  ComPtr<ShellDesktop> current;
  Check(state_->desktops->GetCurrentDesktop(&current), "读取当前桌面");
  GUID current_id{};
  Check(current->GetId(&current_id), "读取当前桌面编号");
  for (UINT i = 0; i < count; ++i) {
    ComPtr<ShellDesktop> desktop;
    Check(array->GetAt(i, IID_PPV_ARGS(&desktop)), "读取桌面");
    GUID id{};
    Check(desktop->GetId(&id), "读取桌面编号");
    const auto id_text = GuidText(id);
    std::wstring name = L"桌面 " + std::to_wstring(i + 1);
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer);
    const auto key =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops\\Desktops\\" +
        Wide(id_text);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Name", RRF_RT_REG_SZ, nullptr, buffer,
                     &size) == ERROR_SUCCESS &&
        buffer[0])
      name = buffer;
    result.push_back(
        {{"id", id_text}, {"number", i + 1}, {"name", Utf8(name)}, {"current", id == current_id}});
  }
  return result;
}
int Desktops::Current() const {
  for (const auto& item : List())
    if (item.at("current").get<bool>()) return item.at("number");
  return 0;
}
int Desktops::WindowDesktop(HWND window) const {
  if (!state_->public_manager) return 0;
  GUID id{};
  if (FAILED(state_->public_manager->GetWindowDesktopId(window, &id))) return 0;
  const auto text = GuidText(id);
  for (const auto& item : List())
    if (item.at("id") == text) return item.at("number");
  return 0;
}
void Desktops::Ensure(int count) {
  if (!available()) throw std::runtime_error(state_->error);
  if (count < 1 || count > 32) throw std::invalid_argument("桌面数量应为 1–32");
  auto current = List().size();
  while (current < static_cast<size_t>(count)) {
    ComPtr<ShellDesktop> created;
    Check(state_->desktops->CreateDesktop(&created), "创建虚拟桌面");
    ++current;
  }
}
void Desktops::Move(HWND window, int number) {
  if (number == 0) number = Current();
  if (number == 0) throw std::runtime_error(state_->error);
  if (WindowDesktop(window) == number) return;
  const auto desktop = state_->At(number);
  ComPtr<ApplicationView> view;
  HRESULT result = E_FAIL;
  for (int i = 0; i < 20; ++i) {
    result = state_->views->GetViewForHwnd(window, &view);
    if (SUCCEEDED(result) && view) break;
    view.Reset();
    Sleep(100);
  }
  Check(result, "获取应用窗口");
  if (!view) throw std::runtime_error("Windows 尚未识别此应用窗口");
  BOOL movable = FALSE;
  Check(state_->desktops->CanViewMoveDesktops(view.Get(), &movable), "检查窗口桌面权限");
  if (!movable) throw std::runtime_error("此窗口不能移动到其他桌面");
  Check(state_->desktops->MoveViewToDesktop(view.Get(), desktop.Get()), "移动窗口到目标桌面");
  for (int i = 0; i < 30; ++i) {
    if (WindowDesktop(window) == number) return;
    Sleep(100);
  }
  throw std::runtime_error("Windows 未确认窗口的桌面位置");
}
void Desktops::Remove(const std::string& id, const std::string& fallback_id) {
  if (!available()) throw std::runtime_error(state_->error);
  if (id == fallback_id) throw std::invalid_argument("不能移除回退桌面");
  GUID target_id{}, home_id{};
  Check(CLSIDFromString(Wide(id).c_str(), &target_id), "桌面编号");
  Check(CLSIDFromString(Wide(fallback_id).c_str(), &home_id), "回退桌面编号");
  ComPtr<ShellDesktop> target, home;
  Check(state_->desktops->FindDesktop(&target_id, &target), "查找待移除桌面");
  Check(state_->desktops->FindDesktop(&home_id, &home), "查找回退桌面");
  Check(state_->desktops->RemoveDesktop(target.Get(), home.Get()), "移除测试桌面");
}
}  // namespace trigger
