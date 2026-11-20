// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:38
// ABOUTME: Validates user scenes and calculates placement within monitor work areas.
#include "core.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
namespace trigger {
namespace {
std::string Text(const Json& value, const char* key, size_t limit, bool empty = false) {
  if (!value.contains(key) || !value[key].is_string())
    throw std::invalid_argument(std::string("缺少文本字段：") + key);
  auto text = value[key].get<std::string>();
  if ((!empty && text.empty()) || text.size() > limit ||
      std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32; }))
    throw std::invalid_argument(std::string("文本字段无效：") + key);
  return text;
}
int Number(const Json& value, const char* key, int minimum, int maximum) {
  if (!value.contains(key) || !value[key].is_number_integer())
    throw std::invalid_argument(std::string("缺少整数：") + key);
  auto number = value[key].get<int64_t>();
  if (number < minimum || number > maximum)
    throw std::invalid_argument(std::string("数值超出范围：") + key);
  return static_cast<int>(number);
}
void Flag(const Json& value, const char* key) {
  if (!value.contains(key) || !value[key].is_boolean())
    throw std::invalid_argument(std::string("开关无效：") + key);
}
}  // namespace

Rect Place(const Layout& layout, const Rect& work_area) {
  if (work_area.width <= 0 || work_area.height <= 0 || !std::isfinite(layout.x) ||
      !std::isfinite(layout.y) || !std::isfinite(layout.width) || !std::isfinite(layout.height) ||
      layout.width <= 0 || layout.height <= 0)
    throw std::invalid_argument("窗口和显示器尺寸必须是有效正数");
  const double width = std::min(1.0, layout.width);
  const double height = std::min(1.0, layout.height);
  const double x = std::clamp(layout.x, 0.0, 1.0 - width);
  const double y = std::clamp(layout.y, 0.0, 1.0 - height);
  const int left = static_cast<int>(std::lround(x * work_area.width));
  const int top = static_cast<int>(std::lround(y * work_area.height));
  const int right = static_cast<int>(std::lround((x + width) * work_area.width));
  const int bottom = static_cast<int>(std::lround((y + height) * work_area.height));
  return {work_area.x + left, work_area.y + top, std::max(1, right - left),
          std::max(1, bottom - top)};
}
Layout Capture(const Rect& window, const Rect& work_area) {
  if (window.width <= 0 || window.height <= 0 || work_area.width <= 0 || work_area.height <= 0)
    throw std::invalid_argument("无法记录空窗口或空显示器");
  return {static_cast<double>(window.x - work_area.x) / work_area.width,
          static_cast<double>(window.y - work_area.y) / work_area.height,
          static_cast<double>(window.width) / work_area.width,
          static_cast<double>(window.height) / work_area.height};
}
std::wstring QuoteArgument(const std::wstring& value) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t character : value) {
    if (character == L'\\') {
      ++slashes;
      continue;
    }
    result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (character == L'"') result += L'\\';
    result += character;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}
bool IsWebUrl(const std::string& value) {
  const size_t start = value.starts_with("https://") ? 8 : value.starts_with("http://") ? 7 : 0;
  if (start == 0 || value.size() <= start || value.size() > 8192) return false;
  if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
    return false;
  const auto end = value.find_first_of("/?#", start);
  const auto authority = value.substr(start, end == std::string::npos ? end : end - start);
  return !authority.empty() && authority.find('@') == std::string::npos &&
         authority.find('\\') == std::string::npos;
}
void ValidateScene(const Json& scene) {
  try {
    Text(scene, "id", 64);
    Text(scene, "name", 160);
    Number(scene, "hotkey", 0, 9);
    if (!scene.contains("entries") || !scene["entries"].is_array() || scene["entries"].empty() ||
        scene["entries"].size() > 64)
      throw std::invalid_argument("每个场景需要 1 至 64 个窗口");
    std::set<std::string> ids;
    for (const auto& entry : scene["entries"]) {
      if (!ids.insert(Text(entry, "id", 64)).second) throw std::invalid_argument("窗口编号重复");
      Text(entry, "name", 160);
      const auto kind = Text(entry, "kind", 8);
      if (kind != "app" && kind != "web") throw std::invalid_argument("窗口类型无效");
      Text(entry, "path", 32700);
      Text(entry, "args", 16000, true);
      Text(entry, "workdir", 32700, true);
      Text(entry, "title", 2048, true);
      Text(entry, "monitor", 2048);
      if (kind == "web" && !IsWebUrl(Text(entry, "url", 8192)))
        throw std::invalid_argument("网页需要完整的 http:// 或 https:// 网址");
      Number(entry, "desktop", 0, 32);
      Flag(entry, "reuse");
      const auto state = Text(entry, "state", 16);
      if (state != "normal" && state != "maximized") throw std::invalid_argument("窗口状态无效");
      const auto& bounds = entry.at("layout");
      Layout layout{bounds.at("x").get<double>(), bounds.at("y").get<double>(),
                    bounds.at("width").get<double>(), bounds.at("height").get<double>()};
      Place(layout, {0, 0, 1000, 1000});
    }
  } catch (const Json::exception&) {
    throw std::invalid_argument("场景字段的类型或格式不正确");
  }
}
void ValidateSettings(const Json& settings) {
  try {
    Number(settings, "version", 1, 1);
    Flag(settings, "otp_enabled");
    Flag(settings, "hotkeys_enabled");
    Flag(settings, "hide_after_launch");
    if (!settings.at("scenes").is_array() || settings["scenes"].size() > 64)
      throw std::invalid_argument("最多保存 64 个场景");
    std::set<std::string> ids;
    std::set<int> hotkeys;
    for (const auto& scene : settings["scenes"]) {
      ValidateScene(scene);
      if (!ids.insert(scene.at("id").get<std::string>()).second)
        throw std::invalid_argument("场景编号重复");
      const int hotkey = scene.at("hotkey");
      if (hotkey && !hotkeys.insert(hotkey).second)
        throw std::invalid_argument("多个场景使用了同一快捷键");
    }
  } catch (const Json::exception&) {
    throw std::invalid_argument("配置字段的类型或格式不正确");
  }
}
std::string Identifier() {
  std::random_device random;
  std::ostringstream stream;
  for (int i = 0; i < 4; ++i) stream << std::hex << std::setw(8) << std::setfill('0') << random();
  return stream.str();
}
Json DefaultSettings(const std::string& browser_path) {
  Json entries = Json::array();
  const char* names[] = {"牛客", "BOSS 直聘"};
  const char* urls[] = {"https://www.nowcoder.com/",
                        "https://www.zhipin.com/web/user/?ka=header-login"};
  for (int i = 0; i < 2; ++i) {
    entries.push_back({{"id", Identifier()},
                       {"name", names[i]},
                       {"kind", "web"},
                       {"path", browser_path},
                       {"url", urls[i]},
                       {"args", ""},
                       {"workdir", ""},
                       {"title", ""},
                       {"monitor", "primary"},
                       {"desktop", 0},
                       {"state", "normal"},
                       {"reuse", true},
                       {"layout", {{"x", i * .5}, {"y", 0}, {"width", .5}, {"height", 1}}}});
  }
  return {{"version", 1},
          {"otp_enabled", true},
          {"hotkeys_enabled", true},
          {"hide_after_launch", true},
          {"scenes",
           Json::array(
               {{{"id", Identifier()}, {"name", "求职"}, {"hotkey", 1}, {"entries", entries}}})}};
}
std::vector<Json> CodeInbox::Recent(int64_t now) {
  std::erase_if(codes_, [now](const Json& code) {
    const double age = static_cast<double>(now) - code["receivedAt"].get<double>();
    return age > 300 || age < -5;
  });
  return codes_;
}
std::vector<Json> CodeInbox::Receive(const Json& events, int64_t now) {
  Recent(now);
  std::vector<Json> fresh;
  if (!events.is_array() || events.size() > 256) return fresh;
  for (const auto& event : events) {
    try {
      const auto id = Text(event, "id", 512);
      const auto code = Text(event, "code", 8);
      const auto sender = Text(event, "sender", 128, true);
      if (code.size() < 4 ||
          !std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; }))
        continue;
      const double received = event.at("receivedAt").get<double>();
      const double age = static_cast<double>(now) - received;
      if (!std::isfinite(received) || age > 300 || age < -5) continue;
      if (std::any_of(codes_.begin(), codes_.end(),
                      [&](const Json& item) { return item["id"] == id; }))
        continue;
      Json item{{"id", id}, {"code", code}, {"sender", sender}, {"receivedAt", received}};
      codes_.push_back(item);
      fresh.push_back(item);
    } catch (const std::exception&) {
      continue;
    }
  }
  auto descending = [](const Json& a, const Json& b) {
    return a["receivedAt"].get<double>() > b["receivedAt"].get<double>();
  };
  std::stable_sort(codes_.begin(), codes_.end(), descending);
  std::stable_sort(fresh.begin(), fresh.end(), descending);
  if (codes_.size() > 32) codes_.resize(32);
  return fresh;
}
}  // namespace trigger
