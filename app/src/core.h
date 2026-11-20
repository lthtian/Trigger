// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:38
// ABOUTME: Scene validation, monitor-relative geometry and short-lived verification codes.
#ifndef TRIGGER_CORE_H_
#define TRIGGER_CORE_H_

#include <cstdint>
#include <json.hpp>
#include <string>
#include <vector>

namespace trigger {
using Json = nlohmann::json;
struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool operator==(const Rect&) const = default;
};
struct Layout {
  double x = 0;
  double y = 0;
  double width = 1;
  double height = 1;
};
Rect Place(const Layout& layout, const Rect& work_area);
Layout Capture(const Rect& window, const Rect& work_area);
std::wstring QuoteArgument(const std::wstring& value);
bool IsWebUrl(const std::string& value);
void ValidateScene(const Json& scene);
void ValidateSettings(const Json& settings);
Json DefaultSettings(const std::string& browser_path);
std::string Identifier();

class CodeInbox {
 public:
  std::vector<Json> Receive(const Json& events, int64_t now);
  std::vector<Json> Recent(int64_t now);

 private:
  std::vector<Json> codes_;
};
}  // namespace trigger
#endif  // TRIGGER_CORE_H_
