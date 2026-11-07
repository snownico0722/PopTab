#include "setup.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {
namespace fs = std::filesystem;
struct Browser { const wchar_t* profiles; const wchar_t* registry; };
constexpr Browser kBrowsers[] = {
  {L"Google\\Chrome\\User Data", L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.poptab.titlepin"},
  {L"Microsoft\\Edge\\User Data", L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.poptab.titlepin"},
  {L"BraveSoftware\\Brave-Browser\\User Data", L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\com.poptab.titlepin"}
};
struct FoundExtension { size_t browser; std::string id; bool native; };
struct Span { size_t start = 0, end = 0; };
void Spaces(std::string_view source, size_t& pos) {
  while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' ||
         source[pos] == '\r' || source[pos] == '\n')) ++pos;
}
std::string Utf8(std::wstring_view text) {
  int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                 nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  if (size) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                               result.data(), size, nullptr, nullptr);
  return result;
}
std::wstring Wide(std::string_view text) {
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(size, L'\0');
  if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()), result.data(), size);
  return result;
}
bool ParseString(std::string_view source, size_t& pos, std::string& value) {
  if (pos >= source.size() || source[pos++] != '"') return false;
  value.clear();
  while (pos < source.size()) {
    char ch = source[pos++];
    if (ch == '"') return true;
    if (ch != '\\') {
      if (static_cast<unsigned char>(ch) < 0x20) return false;
      value += ch;
      continue;
    }
    if (pos >= source.size()) return false;
    ch = source[pos++];
    if (ch == 'u') {
      // Decode escaped UTF-16, including surrogate pairs in profile paths.
      std::wstring units;
      do {
        if (pos + 4 > source.size()) return false;
        unsigned unit = 0;
        const auto parsed = std::from_chars(source.data() + pos, source.data() + pos + 4, unit, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != source.data() + pos + 4) return false;
        units += static_cast<wchar_t>(unit);
        pos += 4;
        if (unit < 0xD800 || unit > 0xDBFF) break;
        if (pos + 2 > source.size() || source.substr(pos, 2) != "\\u") return false;
        pos += 2;
      } while (units.size() == 1);
      value += Utf8(units);
    } else {
      switch (ch) {
        case '\\': case '"': case '/': value += ch; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default: return false;
      }
    }
  }
  return false;
}
bool SkipValue(std::string_view source, size_t& pos, int depth = 0) {
  if (depth > 96) return false;
  Spaces(source, pos);
  if (pos >= source.size()) return false;
  if (source[pos] == '"') { std::string ignored; return ParseString(source, pos, ignored); }
  if (source[pos] == '{' || source[pos] == '[') {
    const bool object = source[pos++] == '{';
    const char end = object ? '}' : ']';
    Spaces(source, pos);
    if (pos < source.size() && source[pos] == end) { ++pos; return true; }
    while (pos < source.size()) {
      if (object) {
        std::string key;
        if (!ParseString(source, pos, key)) return false;
        Spaces(source, pos);
        if (pos >= source.size() || source[pos++] != ':') return false;
      }
      if (!SkipValue(source, pos, depth + 1)) return false;
      Spaces(source, pos);
      if (pos < source.size() && source[pos] == end) { ++pos; return true; }
      if (pos >= source.size() || source[pos++] != ',') return false;
      Spaces(source, pos);
    }
    return false;
  }
  const size_t start = pos;
  while (pos < source.size() && std::string_view(",}] \t\n\r").find(source[pos]) == std::string_view::npos) ++pos;
  return pos > start;
}
template <typename Callback>
bool ForMembers(std::string_view source, Span object, Callback callback) {
  size_t pos = object.start;
  Spaces(source, pos);
  if (pos >= object.end || source[pos++] != '{') return false;
  Spaces(source, pos);
  if (pos < object.end && source[pos] == '}') return true;
  while (pos < object.end) {
    std::string key;
    if (!ParseString(source, pos, key)) return false;
    Spaces(source, pos);
    if (pos >= object.end || source[pos++] != ':') return false;
    Spaces(source, pos);
    size_t start = pos;
    if (!SkipValue(source, pos) || pos > object.end) return false;
    callback(key, Span{start, pos});
    Spaces(source, pos);
    if (pos < object.end && source[pos] == '}') return true;
    if (pos >= object.end || source[pos++] != ',') return false;
    Spaces(source, pos);
  }
  return false;
}
bool Field(std::string_view source, Span object, std::string_view name, Span& output) {
  bool found = false;
  const bool valid = ForMembers(source, object, [&](const std::string& key, Span value) {
    if (key == name) { output = value; found = true; }
  });
  return valid && found;
}
std::string StringValue(std::string_view source, Span span) {
  std::string value;
  size_t pos = span.start;
  return ParseString(source, pos, value) && pos == span.end ? value : "";
}
bool ValidId(std::string_view id) {
  return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'p'; });
}
bool ReadFileUtf8(const fs::path& path, std::string& content) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER length{};
  bool ok = GetFileSizeEx(file, &length) && length.QuadPart >= 0 && length.QuadPart <= 128 * 1024 * 1024;
  if (ok) {
    content.resize(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    ok = content.empty() || (ReadFile(file, content.data(), static_cast<DWORD>(content.size()),
                                     &read, nullptr) && read == content.size());
  }
  CloseHandle(file);
  return ok;
}
bool SaveBytes(const fs::path& path, const void* data, DWORD length) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(file, data, length, &written, nullptr) && written == length;
  CloseHandle(file);
  return ok;
}
std::wstring Environment(const wchar_t* name) {
  DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size) return {};
  std::wstring result(size, L'\0');
  DWORD copied = GetEnvironmentVariableW(name, result.data(), size);
  if (!copied || copied >= size) return {};
  result.resize(copied);
  return result;
}
std::wstring StartupPath() {
  std::wstring path(32768, L'\0');
  DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!size || size >= path.size()) return {};
  path.resize(size);
  return path;
}
bool Enabled(std::string_view json, Span entry) {
  Span value;
  if (Field(json, entry, "state", value) && json.substr(value.start, value.end - value.start) != "1") return false;
  if (Field(json, entry, "disable_reasons", value)) {
    auto reasons = json.substr(value.start, value.end - value.start);
    std::string compact;
    for (char c : reasons) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') compact += c;
    if (compact != "[]" && compact != "0") return false;
  }
  return true;
}
void ScanProfile(std::string_view json, const fs::path& profile, size_t browser,
                 std::set<std::string>& seen, std::vector<FoundExtension>& results) {
  Span extensions, settings;
  if (!Field(json, {0, json.size()}, "extensions", extensions) ||
      !Field(json, extensions, "settings", settings)) return;
  ForMembers(json, settings, [&](const std::string& id, Span entry) {
    if (!ValidId(id) || !seen.insert(id).second || !Enabled(json, entry)) return;
    Span manifest, value;
    std::string diskManifest;
    std::string_view source = json;
    bool hasManifest = Field(json, entry, "manifest", manifest);
    // Unpacked installations can store only a path, not a cached manifest.
    if (Field(json, entry, "path", value)) {
      fs::path path(Wide(StringValue(json, value)));
      if (!path.empty()) {
        if (path.is_relative()) path = profile / L"Extensions" / path;
        if (ReadFileUtf8(path / L"manifest.json", diskManifest)) {
          source = diskManifest;
          manifest = {0, source.size()};
          hasManifest = true;
        }
      }
    }
    Span name, commands, command, permissions;
    if (!hasManifest || !Field(source, manifest, "name", name) || StringValue(source, name) != "PopTab" ||
        !Field(source, manifest, "commands", commands) || !Field(source, commands, "toggle-clean-window", command)) return;
    bool native = false, alarms = false;
    if (Field(source, manifest, "permissions", permissions)) {
      size_t pos = permissions.start;
      if (pos < source.size() && source[pos++] == '[') while (pos < permissions.end) {
        Spaces(source, pos);
        if (pos >= permissions.end || source[pos] == ']') break;
        std::string permission;
        if (!ParseString(source, pos, permission)) break;
        native |= permission == "nativeMessaging";
        alarms |= permission == "alarms";
        Spaces(source, pos);
        if (pos < permissions.end && source[pos] == ',') ++pos; else break;
      }
    }
    native = native && alarms; // The current bridge needs both permissions.
    auto existing = std::find_if(results.begin(), results.end(), [&](const FoundExtension& f) {
      return f.browser == browser && f.id == id;
    });
    if (existing == results.end()) results.push_back({browser, id, native});
    else existing->native |= native;
  });
}
std::vector<FoundExtension> Discover(const fs::path& local) {
  std::vector<FoundExtension> found;
  for (size_t browser = 0; browser < std::size(kBrowsers); ++browser) {
    std::error_code error;
    fs::directory_iterator dirs(local / kBrowsers[browser].profiles, error);
    if (error) continue;
    for (const auto& profile : dirs) {
      if (!profile.is_directory(error)) continue;
      std::set<std::string> seen;
      for (const wchar_t* name : {L"Secure Preferences", L"Preferences"}) {
        std::string json;
        if (ReadFileUtf8(profile.path() / name, json)) ScanProfile(json, profile.path(), browser, seen, found);
      }
    }
  }
  return found;
}
bool ExtractExtension(const fs::path& directory) {
  std::error_code error;
  fs::create_directories(directory / L"assets" / L"icons", error);
  if (error) return false;
  const std::pair<int, const wchar_t*> files[] = {
    {1101, L"manifest.json"}, {1102, L"background.js"}, {1103, L"native-companion.js"},
    {1104, L"video-float.js"}, {1116, L"assets\\icons\\icon-16.png"},
    {1132, L"assets\\icons\\icon-32.png"}, {1148, L"assets\\icons\\icon-48.png"}, {1199, L"assets\\icons\\icon-128.png"}
  };
  for (const auto& [id, filename] : files) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return false;
    HGLOBAL data = LoadResource(nullptr, resource);
    if (!data || !SaveBytes(directory / filename, LockResource(data), SizeofResource(nullptr, resource))) return false;
  }
  return true;
}
std::string JsonString(std::string_view value) {
  std::string output = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') output += '\\';
    output += c;
  }
  return output + '"';
}
bool RegisterHosts(const fs::path& manifest, const fs::path& exe, const std::vector<FoundExtension>& found) {
  std::set<std::string> ids;
  std::set<size_t> browsers;
  for (const auto& extension : found) if (extension.native) {
    ids.insert(extension.id);
    browsers.insert(extension.browser);
  }
  std::string json = "{\"name\":\"com.poptab.titlepin\",\"description\":\"PopTab Windows titlebar pin\","
      "\"type\":\"stdio\",\"path\":" + JsonString(Utf8(exe.wstring())) + ",\"allowed_origins\":[";
  for (const auto& id : ids) {
    if (json.back() != '[') json += ',';
    json += JsonString("chrome-extension://" + id + "/");
  }
  json += "]}";
  if (!SaveBytes(manifest, json.data(), static_cast<DWORD>(json.size()))) return false;
  for (size_t browser : browsers) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kBrowsers[browser].registry, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto path = manifest.wstring();
    LSTATUS result = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()),
                                    static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return false;
  }
  return true;
}
bool StopHosts(const fs::path& installed) {
  // Close only our own message-only dispatcher, never any browser window/process.
  HWND window = nullptr;
  while ((window = FindWindowExW(HWND_MESSAGE, window, L"PopTabPinDispatcher", nullptr))) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) continue;
    wchar_t path[32768]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    bool ours = QueryFullProcessImageNameW(process, 0, path, &size) &&
                _wcsicmp(path, installed.c_str()) == 0;
    if (ours) {
      PostMessageW(window, WM_CLOSE, 0, 0);
      if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) { CloseHandle(process); return false; }
      window = nullptr; // The previous HWND is destroyed; enumerate from the beginning.
    }
    CloseHandle(process);
  }
  return true;
}
void Show(bool silent, const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
  if (!silent) MessageBoxW(nullptr, text.c_str(), L"PopTab Companion", MB_OK | icon);
}
} // namespace

int RunSetup(bool silent, bool uninstall) {
  silent |= uninstall;
  const fs::path local = Environment(L"LOCALAPPDATA");
  const fs::path self = StartupPath();
  if (local.empty() || self.empty()) return 1;
  const fs::path folder = local / L"PopTabCompanion";
  const fs::path installed = folder / L"PopTabCompanion.exe";
  const fs::path manifest = folder / L"native-host.json";
  const fs::path extensionFolder = folder / L"PopTab-extension";
  if (!silent && fs::exists(installed)) {
    const TASKDIALOG_BUTTON buttons[] = {{100, L"安装 / 修复"}, {101, L"卸载辅助程序"}};
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.pszWindowTitle = L"PopTab Companion";
    dialog.pszMainInstruction = L"PopTab 标题栏置顶";
    dialog.pszContent = L"卸载只移除辅助程序和通信配置，保留浏览器扩展。";
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.cButtons = 2;
    dialog.pButtons = buttons;
    dialog.nDefaultButton = 100;
    int selected = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr)) || selected == IDCANCEL) return 0;
    uninstall = selected == 101;
  }
  if (uninstall) {
    for (const auto& browser : kBrowsers) RegDeleteKeyW(HKEY_CURRENT_USER, browser.registry);
    if (!StopHosts(installed)) { Show(silent, L"辅助程序暂未退出，请关闭 PopTab 弹窗后重试。", MB_ICONERROR); return 4; }
    std::error_code error;
    fs::remove(manifest, error);
    if (_wcsicmp(self.c_str(), installed.c_str()) != 0) {
      fs::remove(installed, error);
      if (error) { Show(silent, L"通信已注销，但程序文件仍被占用。关闭后可手动删除。", MB_ICONWARNING); return 4; }
    }
    Show(silent, L"辅助程序已卸载，浏览器扩展已保留。\n若正在运行的是安装目录中的 EXE，关闭后可自行删除该文件。");
    return 0;
  }
  std::error_code error;
  fs::create_directories(folder, error);
  if (error) return 2;
  // Stop the host before touching the EXE or the embedded extension files.
  if (!StopHosts(installed) || (_wcsicmp(self.c_str(), installed.c_str()) != 0 &&
      !CopyFileW(self.c_str(), installed.c_str(), FALSE))) {
    Show(silent, L"无法更新辅助程序。请关闭 PopTab 弹窗后重试；浏览器不会被强制结束。", MB_ICONERROR);
    return 4;
  }
  if (!ExtractExtension(extensionFolder)) { Show(silent, L"无法解压 PopTab 扩展。", MB_ICONERROR); return 3; }
  const auto found = Discover(local);
  const auto ready = std::count_if(found.begin(), found.end(), [](const FoundExtension& f) { return f.native; });
  if (!ready) {
    Show(silent, L"未检测到已启用且支持原生通信的 PopTab。\n\n请在浏览器扩展管理页启用开发者模式，选择“加载已解压的扩展程序”：\n" +
         extensionFolder.wstring() + L"\n\n加载后再次双击本 EXE。程序不会修改浏览器配置文件。");
    if (!silent) ShellExecuteW(nullptr, L"open", extensionFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return found.empty() ? 7 : 8;
  }
  if (!RegisterHosts(manifest, installed, found)) { Show(silent, L"无法注册浏览器通信。", MB_ICONERROR); return 5; }
  Show(silent, L"安装完成。\n\n刚更新扩展时，请在浏览器扩展页重新加载一次。\n打开或激活 PopTab 弹窗即可使用标题栏图钉。\n已打开的弹窗会在下一次重连时恢复，通常不超过一分钟。");
  return 0;
}
