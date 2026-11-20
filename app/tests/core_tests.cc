// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:38
// ABOUTME: Exercises production geometry, scene validation, quoting and code expiry.
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "core.h"

using namespace trigger;
void Check(bool value) {
  if (!value) throw std::runtime_error("assertion failed");
}
template <typename F>
void Reject(F function) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected);
}
int main() {
  int failures = 0;
  int count = 0;
  auto test = [&](const char* name, const std::function<void()>& function) {
    ++count;
    try {
      function();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cout << "FAIL " << name << ": " << error.what() << '\n';
    }
  };
  test("primary screen halves", [] {
    Check(Place({0, 0, .5, 1}, {0, 0, 1920, 1040}) == Rect{0, 0, 960, 1040});
    Check(Place({.5, 0, .5, 1}, {0, 0, 1920, 1040}) == Rect{960, 0, 960, 1040});
  });
  test("negative monitor origin and work area", [] {
    Check(Place({.5, 0, .5, 1}, {-2560, 40, 2560, 1400}) == Rect{-1280, 40, 1280, 1400});
  });
  test("odd pixel split covers work area", [] {
    auto left = Place({0, 0, .5, 1}, {0, 0, 1537, 900});
    auto right = Place({.5, 0, .5, 1}, {0, 0, 1537, 900});
    Check(left.width == right.x && right.x + right.width == 1537);
  });
  test("offscreen layout clamped",
       [] { Check(Place({2, -1, .5, .5}, {0, 0, 1000, 800}) == Rect{500, 0, 500, 400}); });
  test("capture survives resolution changes", [] {
    auto layout = Capture({1000, 100, 800, 800}, {200, 100, 1600, 800});
    Check(Place(layout, {-1920, 0, 1920, 1080}) == Rect{-960, 0, 960, 1080});
  });
  test("invalid geometry rejected", [] {
    Reject([] { Place({0, 0, 0, 1}, {0, 0, 100, 100}); });
    Reject([] { Place({0, 0, 1, 1}, {0, 0, 0, 100}); });
    Reject([] { Place({0, 0, std::numeric_limits<double>::quiet_NaN(), 1}, {0, 0, 100, 100}); });
  });
  test("argument quoting", [] {
    Check(QuoteArgument(L"") == L"\"\"");
    Check(QuoteArgument(L"C:\\a b\\") == L"\"C:\\a b\\\\\"");
    Check(QuoteArgument(L"a\"b") == L"\"a\\\"b\"");
  });
  test("URL scheme and control characters", [] {
    Check(IsWebUrl("https://www.nowcoder.com/"));
    Check(IsWebUrl("http://localhost:8080/a?q=1"));
    Check(!IsWebUrl("file:///C:/Windows"));
    Check(!IsWebUrl("https://"));
    Check(!IsWebUrl("https://a\r\nx"));
    Check(!IsWebUrl("https://a b/"));
  });
  test("default scenes valid", [] {
    auto settings = DefaultSettings("C:/browser.exe");
    ValidateSettings(settings);
    Check(settings.at("scenes").at(0).at("entries").size() == 2);
  });
  test("stable identifier shape", [] {
    const auto first = Identifier(), second = Identifier();
    Check(first.size() == 32 && second.size() == 32 && first != second);
  });
  test("malformed and empty scenes rejected", [] {
    Reject([] { ValidateScene(Json::object()); });
    auto scene = DefaultSettings("browser.exe")["scenes"][0];
    scene["entries"] = Json::array();
    Reject([&] { ValidateScene(scene); });
  });
  test("duplicate entry ids rejected", [] {
    auto scene = DefaultSettings("browser.exe")["scenes"][0];
    scene["entries"][1]["id"] = scene["entries"][0]["id"];
    Reject([&] { ValidateScene(scene); });
  });
  test("desktop and hotkey bounds", [] {
    auto scene = DefaultSettings("browser.exe")["scenes"][0];
    scene["entries"][0]["desktop"] = 33;
    Reject([&] { ValidateScene(scene); });
    scene["entries"][0]["desktop"] = 0;
    scene["hotkey"] = 10;
    Reject([&] { ValidateScene(scene); });
  });
  test("application fields validated", [] {
    auto scene = DefaultSettings("browser.exe")["scenes"][0];
    scene["entries"][0]["kind"] = "app";
    scene["entries"][0]["path"] = "";
    Reject([&] { ValidateScene(scene); });
  });
  test("duplicate scene hotkeys rejected", [] {
    auto settings = DefaultSettings("browser.exe");
    auto another = settings["scenes"][0];
    another["id"] = Identifier();
    settings["scenes"].push_back(another);
    Reject([&] { ValidateSettings(settings); });
  });
  test("codes expire and are bounded", [] {
    CodeInbox inbox;
    Json events = Json::array();
    for (int i = 0; i < 40; ++i)
      events.push_back({{"id", std::to_string(i)},
                        {"sender", "test"},
                        {"code", "123456"},
                        {"receivedAt", 1000 + i}});
    auto fresh = inbox.Receive(events, 1040);
    Check(!fresh.empty() && inbox.Recent(1040).size() == 32);
    Check(inbox.Recent(1400).empty());
  });
  test("code delivery deduplicated and malformed ignored", [] {
    CodeInbox inbox;
    Json events =
        Json::array({{{"id", "a"}, {"sender", "test"}, {"code", "1234"}, {"receivedAt", 1000}},
                     {{"id", "b"}, {"code", "not-a-code"}, {"receivedAt", 1000}},
                     {{"id", "c"}, {"code", "5678"}, {"receivedAt", 699}},
                     {{"id", "d"}, {"code", "5678"}, {"receivedAt", 1100}}});
    Check(inbox.Receive(events, 1000).size() == 1);
    Check(inbox.Receive(events, 1001).empty());
    Check(inbox.Recent(1001).size() == 1);
  });
  std::cout << count - failures << '/' << count << " tests passed\n";
  return failures ? 1 : 0;
}
