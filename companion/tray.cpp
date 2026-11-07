#include "tray.h"
#include "setup.h"

#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <string>
#include <cwchar>
#include <iterator>

namespace {
constexpr wchar_t kTrayTitle[] = L"PopTab Companion";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kSettingsKey[] = L"Software\\PopTabCompanion";
constexpr wchar_t kStartupValue[] = L"AutoStart";
constexpr UINT kTrayMessage = WM_APP + 40;
constexpr UINT kToggleStartup = 401;
constexpr UINT kOpenFolder = 402;
constexpr UINT kExit = 403;
constexpr UINT kTrayId = 1;
UINT g_taskbarCreated = 0;
HICON g_icon = nullptr;
HWND g_trayWindow = nullptr;

std::filesystem::path Executable() {
  std::wstring buffer(32768, L'\0');
  DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                  static_cast<DWORD>(buffer.size()));
  if (!size || size >= buffer.size()) return {};
  buffer.resize(size);
  return buffer;
}

bool Preference(bool& value) {
  DWORD enabled = 0, type = 0, bytes = sizeof(enabled);
  LSTATUS code = RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kStartupValue,
                              RRF_RT_REG_DWORD, &type, &enabled, &bytes);
  if (code != ERROR_SUCCESS) return false;
  value = enabled != 0;
  return true;
}

bool UpdateRunValue(bool enabled, const std::filesystem::path& installed) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  LSTATUS result;
  if (enabled) {
    std::wstring value = L"\"" + installed.wstring() + L"\" --tray";
    result = RegSetValueExW(
        key, L"PopTabCompanion", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  } else {
    result = RegDeleteValueW(key, L"PopTabCompanion");
    if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

HICON MakeTrayIcon() {
  // Use the app's existing 32px icon, embedded in the one-file EXE.
  HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(1132), RT_RCDATA);
  if (!resource) return LoadIconW(nullptr, IDI_APPLICATION);
  HGLOBAL data = LoadResource(nullptr, resource);
  const DWORD size = SizeofResource(nullptr, resource);
  if (!data || !size) return LoadIconW(nullptr, IDI_APPLICATION);
  HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!copy) return LoadIconW(nullptr, IDI_APPLICATION);
  void* bytes = GlobalLock(copy);
  if (!bytes) {
    GlobalFree(copy);
    return LoadIconW(nullptr, IDI_APPLICATION);
  }
  CopyMemory(bytes, LockResource(data), size);
  GlobalUnlock(copy);
  IStream* stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream))) {
    GlobalFree(copy);
    return LoadIconW(nullptr, IDI_APPLICATION);
  }
  HICON icon = nullptr;
  {
    Gdiplus::Bitmap bitmap(stream);
    if (bitmap.GetLastStatus() == Gdiplus::Ok)
      bitmap.GetHICON(&icon);
  }
  stream->Release();
  return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

NOTIFYICONDATAW TrayData(HWND window) {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window;
  data.uID = kTrayId;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = kTrayMessage;
  data.hIcon = g_icon;
  wcscpy_s(data.szTip, kTrayTitle);
  return data;
}

void ShowMenu(HWND window) {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING | (AutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED),
              kToggleStartup, L"开机自启动");
  AppendMenuW(menu, MF_STRING, kOpenFolder, L"打开文件位置");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExit, L"退出");
  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(window);
  const int command = TrackPopupMenu(
      menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
      cursor.x, cursor.y, 0, window, nullptr);
  DestroyMenu(menu);
  if (command) PostMessageW(window, WM_COMMAND, command, 0);
  PostMessageW(window, WM_NULL, 0, 0);
}

void QuitTray(HWND window) {
  // Keep the tray available if a native host did not actually stop.
  if (!StopHostsForCompanion(Executable())) {
    MessageBoxW(window, L"置顶服务尚未退出，请稍后重试。",
                kTrayTitle, MB_OK | MB_ICONWARNING);
    return;
  }
  DestroyWindow(window);
}

LRESULT CALLBACK TrayProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (g_taskbarCreated && message == g_taskbarCreated) {
    auto data = TrayData(window);
    Shell_NotifyIconW(NIM_ADD, &data);
    return 0;
  }
  switch (message) {
    case kTrayMessage:
      if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU ||
          lp == WM_LBUTTONUP) {
        ShowMenu(window);
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kToggleStartup:
          if (!SetAutoStart(!AutoStartEnabled(), Executable())) {
            MessageBoxW(window, L"无法修改开机自启动设置。",
                        kTrayTitle, MB_OK | MB_ICONWARNING);
          }
          return 0;
        case kOpenFolder: {
          const std::filesystem::path path = Executable().parent_path();
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
          return 0;
        }
        case kExit:
          // Quit the whole companion, but keep the explicit startup preference.
          QuitTray(window);
          return 0;
      }
      break;
    case WM_CLOSE:
      QuitTray(window);
      return 0;
    case WM_DESTROY: {
      auto data = TrayData(window);
      Shell_NotifyIconW(NIM_DELETE, &data);
      g_trayWindow = nullptr;
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(window, message, wp, lp);
}
} // namespace

bool AutoStartEnabled() {
  bool enabled = false;
  return Preference(enabled) && enabled;
}

bool SetAutoStart(bool enabled, const std::filesystem::path& installed) {
  if (!UpdateRunValue(enabled, installed)) return false;
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  DWORD value = enabled ? 1 : 0;
  LSTATUS status = RegSetValueExW(key, kStartupValue, 0, REG_DWORD,
                                 reinterpret_cast<const BYTE*>(&value),
                                 sizeof(value));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool ConfigureAutoStart(const std::filesystem::path& installed) {
  bool enabled = true; // First installation defaults to on.
  Preference(enabled); // Repair respects the user's explicit selection.
  return SetAutoStart(enabled, installed);
}

void RemoveAutoStart() {
  UpdateRunValue(false, {});
  RegDeleteKeyW(HKEY_CURRENT_USER, kSettingsKey);
}

bool IsTrayRunning() {
  return FindWindowW(kPopTabTrayClass, kTrayTitle) != nullptr;
}

bool StopTray(const std::filesystem::path& installed) {
  const HWND window = FindWindowW(kPopTabTrayClass, kTrayTitle);
  if (!window) return true;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                               FALSE, pid);
  if (!process) return false;
  wchar_t path[32768]{};
  DWORD count = static_cast<DWORD>(std::size(path));
  const bool ours = QueryFullProcessImageNameW(process, 0, path, &count) &&
                    _wcsicmp(path, installed.c_str()) == 0;
  if (!ours) {
    CloseHandle(process);
    return false;
  }
  PostMessageW(window, WM_CLOSE, 0, 0);
  bool stopped = WaitForSingleObject(process, 3000) == WAIT_OBJECT_0;
  CloseHandle(process);
  return stopped;
}

bool StartTray(const std::filesystem::path& installed) {
  if (IsTrayRunning()) return true;
  return reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", installed.c_str(), L"--tray",
      installed.parent_path().c_str(), SW_SHOWNORMAL)) > 32;
}

int RunTray(HINSTANCE instance) {
  // A single icon is shared by all browser Native Messaging instances.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\PopTabCompanion.Tray");
  if (!mutex) return 1;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return 0;
  }

  ULONG_PTR graphicsToken = 0;
  Gdiplus::GdiplusStartupInput startup;
  if (Gdiplus::GdiplusStartup(&graphicsToken, &startup, nullptr) != Gdiplus::Ok) {
    CloseHandle(mutex);
    return 1;
  }
  g_icon = MakeTrayIcon();
  g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  WNDCLASSW cls{};
  cls.hInstance = instance;
  cls.lpfnWndProc = TrayProc;
  cls.lpszClassName = kPopTabTrayClass;
  if (!RegisterClassW(&cls)) {
    Gdiplus::GdiplusShutdown(graphicsToken);
    CloseHandle(mutex);
    return 1;
  }
  g_trayWindow = CreateWindowExW(
      WS_EX_TOOLWINDOW, kPopTabTrayClass, kTrayTitle, WS_OVERLAPPED,
      0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (!g_trayWindow) {
    Gdiplus::GdiplusShutdown(graphicsToken);
    CloseHandle(mutex);
    return 1;
  }
  auto data = TrayData(g_trayWindow);
  // Explorer can be starting during logon. Keep the tray service running;
  // TaskbarCreated will add the icon when Explorer becomes available.
  Shell_NotifyIconW(NIM_ADD, &data);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  // The icon can be a shared system fallback. Destroy only our GDI+ icon.
  if (g_icon && g_icon != LoadIconW(nullptr, IDI_APPLICATION))
    DestroyIcon(g_icon);
  g_icon = nullptr;
  Gdiplus::GdiplusShutdown(graphicsToken);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}
