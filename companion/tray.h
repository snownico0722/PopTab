#pragma once
#include <windows.h>
#include <filesystem>

constexpr wchar_t kPopTabTrayClass[] = L"PopTabCompanionTrayWindow";
int RunTray(HINSTANCE instance);
bool IsTrayRunning();
bool StopTray(const std::filesystem::path& installed);
bool StartTray(const std::filesystem::path& installed);
bool ConfigureAutoStart(const std::filesystem::path& installed);
bool SetAutoStart(bool enabled, const std::filesystem::path& installed);
bool AutoStartEnabled();
void RemoveAutoStart();
