// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/22 21:50
// ABOUTME: Native monitor inventory, application restoration and private local storage.
#ifndef TRIGGER_PLATFORM_H_
#define TRIGGER_PLATFORM_H_
#include <atomic>
#include <filesystem>
#include <functional>

#include "core.h"
#include "win.h"
namespace trigger {
std::filesystem::path ExecutablePath();
std::filesystem::path DataDirectory();
Json ReadData(const std::filesystem::path& path);
void WriteData(const std::filesystem::path& path, const Json& value);
void SaveConnection(const std::filesystem::path& path, const Json& connection);
Json LoadConnection(const std::filesystem::path& path);
Json FetchEvents(const Json& connection);
Json Monitors();
Json Browsers();
Json Windows();
Json EntryFromWindow(const Json& window);
Json Restore(const Json& scene, Json& bindings,
             const std::function<void(const Json&)>& progress = {},
             const std::atomic_bool* cancel = nullptr);
std::filesystem::path CreateShortcut(const Json& scene, const std::filesystem::path& directory,
                                     const std::filesystem::path& data_directory);
std::wstring PickFile(HWND owner, bool folder = false);
void CopyText(HWND owner, const std::wstring& text);
}  // namespace trigger
#endif  // TRIGGER_PLATFORM_H_
