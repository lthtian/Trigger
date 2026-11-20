// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:47
// ABOUTME: UTF conversions, Windows error reporting and kernel handle ownership.
#ifndef TRIGGER_WIN_H_
#define TRIGGER_WIN_H_
#include <windows.h>
#include <combaseapi.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
namespace trigger {
inline std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (!length) throw std::invalid_argument("文本不是有效 UTF-8");
  std::wstring result(length, L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                           static_cast<int>(text.size()), result.data(), length))
    throw std::runtime_error("无法转换文本");
  return result;
}
inline std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int length =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          nullptr, 0, nullptr, nullptr);
  if (!length) throw std::invalid_argument("文本不是有效 Unicode");
  std::string result(length, '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                           static_cast<int>(text.size()), result.data(), length, nullptr, nullptr))
    throw std::runtime_error("无法转换文本");
  return result;
}
inline void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    std::ostringstream message;
    message << operation << " (0x" << std::hex << static_cast<unsigned long>(result) << ')';
    throw std::runtime_error(message.str());
  }
}
inline void CheckWin(BOOL result, const char* operation) {
  if (!result) {
    const DWORD error = GetLastError();
    Check(error ? HRESULT_FROM_WIN32(error) : E_FAIL, operation);
  }
}
class Handle {
 public:
  explicit Handle(HANDLE value = nullptr) : value_(value) {}
  ~Handle() {
    if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  HANDLE get() const { return value_; }
  explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE value_;
};
inline std::string GuidText(const GUID& id) {
  wchar_t text[40]{};
  if (!StringFromGUID2(id, text, 40)) throw std::runtime_error("无法格式化桌面编号");
  return Utf8(text);
}
}  // namespace trigger
#endif  // TRIGGER_WIN_H_
