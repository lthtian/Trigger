// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:45
// ABOUTME: Windows virtual desktop discovery and window assignment on verified builds.
#ifndef TRIGGER_DESKTOPS_H_
#define TRIGGER_DESKTOPS_H_
#include <windows.h>

#include <memory>

#include "core.h"
namespace trigger {
class Desktops {
 public:
  Desktops();
  ~Desktops();
  Desktops(const Desktops&) = delete;
  Desktops& operator=(const Desktops&) = delete;
  bool available() const;
  std::string error() const;
  Json List() const;
  int Current() const;
  int WindowDesktop(HWND window) const;
  void Ensure(int count);
  void Move(HWND window, int number);
  void Remove(const std::string& id, const std::string& fallback_id);

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace trigger
#endif  // TRIGGER_DESKTOPS_H_
