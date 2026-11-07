#include "setup.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kHostName[] = L"com.poptab.titlepin";
constexpr wchar_t kFolderName[] = L"PopTabCompanion";
constexpr size_t kMaxPreferencesBytes = 128 * 1024 * 1024;

struct Browser {
  const wchar_t* name;
  const wchar_t* profiles;
  const wchar_t* registry;
};

constexpr Browser kBrowsers[] = {
    {L"Chrome", L"Google\\Chrome\\User Data",
     L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.poptab.titlepin"},
    {L"Edge", L"Microsoft\\Edge\\User Data",
     L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.poptab.titlepin"},
    {L"Brave", L"BraveSoftware\\Brave-Browser\\User Data",
     L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\com.poptab.titlepin"},
};

struct FoundExtension {
  size_t browserIndex = 0;
  std::string id;
  bool supportsNative = false;
};

struct Span {
  size_t start = 0;
  size_t end = 0;
};

void Spaces(std::string_view source, size_t& pos) {
  while (pos < source.size() &&
         (source[pos] == ' ' || source[pos] == '\t' ||
          source[pos] == '\r' || source[pos] == '\n')) ++pos;
}

bool ParseString(std::string_view source, size_t& pos, std::string& value) {
  if (pos >= source.size() || source[pos++] != '"') return false;
  value.clear();
  while (pos < source.size()) {
    const char ch = source[pos++];
    if (ch == '"') return true;
    if (ch == '\\') {
      if (pos >= source.size()) return false;
      const char next = source[pos++];
      if (next == 'u') {
        if (pos + 4 > source.size()) return false;
        pos += 4;
        value.push_back('?'); // Only ASCII names and extension IDs are inspected.
      } else {
        switch (next) {
          case '\\': case '"': case '/': value.push_back(next); break;
          case 'b': value.push_back('\b'); break;
          case 'f': value.push_back('\f'); break;
          case 'n': value.push_back('\n'); break;
          case 'r': value.push_back('\r'); break;
          case 't': value.push_back('\t'); break;
          default: return false;
        }
      }
    } else {
      if (static_cast<unsigned char>(ch) < 0x20) return false;
      value.push_back(ch);
    }
  }
  return false;
}

bool SkipValue(std::string_view source, size_t& pos, int depth = 0) {
  if (depth > 96) return false;
  Spaces(source, pos);
  if (pos >= source.size()) return false;
  if (source[pos] == '"') {
    std::string ignored;
    return ParseString(source, pos, ignored);
  }
  if (source[pos] == '{') {
    ++pos;
    Spaces(source, pos);
    if (pos < source.size() && source[pos] == '}') { ++pos; return true; }
    while (pos < source.size()) {
      std::string key;
      if (!ParseString(source, pos, key)) return false;
      Spaces(source, pos);
      if (pos >= source.size() || source[pos++] != ':') return false;
      if (!SkipValue(source, pos, depth + 1)) return false;
      Spaces(source, pos);
      if (pos < source.size() && source[pos] == '}') { ++pos; return true; }
      if (pos >= source.size() || source[pos++] != ',') return false;
      Spaces(source, pos);
    }
    return false;
  }
  if (source[pos] == '[') {
    ++pos;
    Spaces(source, pos);
    if (pos < source.size() && source[pos] == ']') { ++pos; return true; }
    while (pos < source.size()) {
      if (!SkipValue(source, pos, depth + 1)) return false;
      Spaces(source, pos);
      if (pos < source.size() && source[pos] == ']') { ++pos; return true; }
      if (pos >= source.size() || source[pos++] != ',') return false;
    }
    return false;
  }
  const size_t start = pos;
  while (pos < source.size() && source[pos] != ',' &&
         source[pos] != '}' && source[pos] != ']' &&
         source[pos] != ' ' && source[pos] != '\t' &&
         source[pos] != '\n' && source[pos] != '\r') ++pos;
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
    const size_t start = pos;
    if (!SkipValue(source, pos)) return false;
    if (pos > object.end) return false;
    callback(key, Span{start, pos});
    Spaces(source, pos);
    if (pos < object.end && source[pos] == '}') return true;
    if (pos >= object.end || source[pos++] != ',') return false;
    Spaces(source, pos);
  }
  return false;
}

bool Field(std::string_view source, Span object, std::string_view name,
           Span& output) {
  bool found = false;
  ForMembers(source, object, [&](const std::string& key, Span value) {
    if (!found && key == name) {
      output = value;
      found = true;
    }
  });
  return found;
}

bool StringEquals(std::string_view source, Span span, std::string_view value) {
  size_t pos = span.start;
  std::string decoded;
  return ParseString(source, pos, decoded) && pos == span.end &&
         decoded == value;
}

template <typename Callback>
bool ForArray(std::string_view source, Span array, Callback callback) {
  size_t pos = array.start;
  Spaces(source, pos);
  if (pos >= array.end || source[pos++] != '[') return false;
  Spaces(source, pos);
  if (pos < array.end && source[pos] == ']') return true;
  while (pos < array.end) {
    const size_t start = pos;
    if (!SkipValue(source, pos) || pos > array.end) return false;
    callback(Span{start, pos});
    Spaces(source, pos);
    if (pos < array.end && source[pos] == ']') return true;
    if (pos >= array.end || source[pos++] != ',') return false;
    Spaces(source, pos);
  }
  return false;
}

bool ValidId(std::string_view id) {
  if (id.size() != 32) return false;
  return std::all_of(id.begin(), id.end(),
                     [](char c) { return c >= 'a' && c <= 'p'; });
}

bool ReadFileUtf8(const std::filesystem::path& path, std::string& content) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER length{};
  bool ok = GetFileSizeEx(file, &length) && length.QuadPart >= 0 &&
            length.QuadPart <= static_cast<LONGLONG>(kMaxPreferencesBytes);
  if (ok) {
    content.resize(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    ok = content.empty() ||
         (ReadFile(file, content.data(), static_cast<DWORD>(content.size()),
                   &read, nullptr) && read == content.size());
  }
  CloseHandle(file);
  return ok;
}

bool SaveBytes(const std::filesystem::path& path, const void* data,
               size_t length) {
  if (length > UINT32_MAX) return false;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, data, static_cast<DWORD>(length),
                            &written, nullptr) && written == length;
  CloseHandle(file);
  return ok;
}

std::wstring LocalAppData() {
  DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (size <= 1 || size > 32768) return {};
  std::wstring result(size, L'\0');
  const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA",
                                               result.data(), size);
  if (!copied || copied >= size) return {};
  result.resize(copied);
  return result;
}

std::string Utf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int required = WideCharToMultiByte(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (required <= 0) return {};
  std::string result(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(),
                      static_cast<int>(text.size()), result.data(), required,
                      nullptr, nullptr);
  return result;
}

std::string JsonString(std::string_view value) {
  const char digits[] = "0123456789abcdef";
  std::string encoded = "\"";
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') {
      encoded += '\\';
      encoded += static_cast<char>(c);
    } else if (c < 0x20) {
      encoded += "\\u00";
      encoded += digits[c >> 4];
      encoded += digits[c & 15];
    } else {
      encoded += static_cast<char>(c);
    }
  }
  encoded += '"';
  return encoded;
}

bool HasPermission(std::string_view json, Span manifest) {
  Span permissions;
  if (!Field(json, manifest, "permissions", permissions)) return false;
  bool found = false;
  ForArray(json, permissions, [&](Span value) {
    if (StringEquals(json, value, "nativeMessaging")) found = true;
  });
  return found;
}

void ScanProfile(std::string_view json, size_t browser,
                 std::vector<FoundExtension>& results) {
  Span extensions, settings;
  const Span whole{0, json.size()};
  if (!Field(json, whole, "extensions", extensions) ||
      !Field(json, extensions, "settings", settings)) return;
  ForMembers(json, settings, [&](const std::string& id, Span entry) {
    if (!ValidId(id)) return;
    Span manifest, name, commands, command;
    if (!Field(json, entry, "manifest", manifest) ||
        !Field(json, manifest, "name", name) ||
        !StringEquals(json, name, "PopTab") ||
        !Field(json, manifest, "commands", commands) ||
        !Field(json, commands, "toggle-clean-window", command)) return;
    const FoundExtension found{browser, id, HasPermission(json, manifest)};
    auto existing = std::find_if(results.begin(), results.end(),
        [&](const FoundExtension& a) {
          return a.browserIndex == browser && a.id == id;
        });
    if (existing == results.end()) results.push_back(found);
    else existing->supportsNative |= found.supportsNative;
  });
}

std::vector<FoundExtension> Discover(const std::filesystem::path& local) {
  std::vector<FoundExtension> result;
  for (size_t browser = 0; browser < std::size(kBrowsers); ++browser) {
    const auto folder = local / kBrowsers[browser].profiles;
    std::error_code error;
    std::filesystem::directory_iterator dirs(folder, error);
    if (error) continue;
    for (const auto& profile : dirs) {
      if (!profile.is_directory(error)) continue;
      std::string preferences;
      if (!ReadFileUtf8(profile.path() / L"Preferences", preferences)) continue;
      ScanProfile(preferences, browser, result);
    }
  }
  return result;
}

bool ExtractResource(int id, const std::filesystem::path& path) {
  HRSRC info = FindResourceW(GetModuleHandleW(nullptr),
                             MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!info) return false;
  HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), info);
  if (!loaded) return false;
  const void* memory = LockResource(loaded);
  const DWORD size = SizeofResource(GetModuleHandleW(nullptr), info);
  return memory && size && SaveBytes(path, memory, size);
}

bool ExtractExtension(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory / L"assets" / L"icons", error);
  if (error) return false;
  const std::pair<int, const wchar_t*> files[] = {
      {1101, L"manifest.json"},
      {1102, L"background.js"},
      {1103, L"native-companion.js"},
      {1104, L"video-float.js"},
      {1116, L"assets\\icons\\icon-16.png"},
      {1132, L"assets\\icons\\icon-32.png"},
      {1148, L"assets\\icons\\icon-48.png"},
      {1199, L"assets\\icons\\icon-128.png"},
  };
  for (const auto& item : files) {
    if (!ExtractResource(item.first, directory / item.second)) return false;
  }
  return true;
}

bool RegisterHost(const Browser& browser,
                  const std::filesystem::path& manifest) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, browser.registry, 0, nullptr, 0,
                       KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const std::wstring path = manifest.wstring();
  const LSTATUS result = RegSetValueExW(
      key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()),
      static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

std::set<std::string> ExistingOrigins(const std::filesystem::path& path) {
  std::set<std::string> ids;
  std::string json;
  if (!ReadFileUtf8(path, json)) return ids;
  Span origins;
  if (!Field(json, {0, json.size()}, "allowed_origins", origins)) return ids;
  ForArray(json, origins, [&](Span value) {
    size_t pos = value.start;
    std::string origin;
    if (!ParseString(json, pos, origin) || pos != value.end) return;
    constexpr std::string_view prefix = "chrome-extension://";
    if (origin.size() != prefix.size() + 33 ||
        origin.compare(0, prefix.size(), prefix) != 0 ||
        origin.back() != '/') return;
    const std::string_view id(origin.data() + prefix.size(), 32);
    if (ValidId(id)) ids.emplace(id);
  });
  return ids;
}

bool SaveHostManifest(const std::filesystem::path& path,
                      const std::filesystem::path& executable,
                      const std::set<std::string>& ids) {
  std::string json =
      "{\"name\":\"com.poptab.titlepin\","
      "\"description\":\"PopTab Windows titlebar pin\","
      "\"type\":\"stdio\",\"path\":" +
      JsonString(Utf8(executable.wstring())) + ",\"allowed_origins\":[";
  bool first = true;
  for (const auto& id : ids) {
    if (!first) json += ',';
    first = false;
    json += JsonString("chrome-extension://" + id + "/");
  }
  json += "]}";
  return SaveBytes(path, json.data(), json.size());
}

std::wstring StartupPath() {
  std::wstring path(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, path.data(),
                                       static_cast<DWORD>(path.size()));
  if (!size || size >= path.size()) return {};
  path.resize(size);
  return path;
}

void ShowMessage(bool silent, const std::wstring& message, UINT type) {
  if (!silent) MessageBoxW(nullptr, message.c_str(), L"PopTab Companion",
                           type | MB_SETFOREGROUND);
}

} // namespace

int RunSetup(bool silent) {
  const std::wstring local = LocalAppData();
  const std::wstring self = StartupPath();
  if (local.empty() || self.empty()) {
    ShowMessage(silent, L"无法确定程序位置或用户数据目录。", MB_ICONERROR);
    return 1;
  }

  const std::filesystem::path folder =
      std::filesystem::path(local) / kFolderName;
  const std::filesystem::path extensionFolder = folder / L"PopTab-extension";
  const std::filesystem::path installed = folder / L"PopTabCompanion.exe";
  const std::filesystem::path manifest = folder / L"native-host.json";
  const auto extensions = Discover(local);
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  if (error) {
    ShowMessage(silent, L"无法在当前用户目录创建 PopTab 文件夹。", MB_ICONERROR);
    return 2;
  }

  if (!ExtractExtension(extensionFolder)) {
    ShowMessage(silent, L"无法解压随 EXE 附带的 PopTab 扩展。", MB_ICONERROR);
    return 3;
  }

  const std::filesystem::path selfPath = self;
  if (selfPath != installed &&
      !CopyFileW(selfPath.c_str(), installed.c_str(), FALSE)) {
    ShowMessage(silent,
      L"无法安装辅助程序。若它正在运行，请关闭浏览器后重新双击本 EXE。",
      MB_ICONERROR);
    return 4;
  }

  std::set<std::string> ids = ExistingOrigins(manifest);
  bool browsersFound[std::size(kBrowsers)]{};
  size_t outdated = 0;
  for (const auto& extension : extensions) {
    browsersFound[extension.browserIndex] = true;
    ids.insert(extension.id);
    if (!extension.supportsNative) ++outdated;
  }

  if (!extensions.empty()) {
    if (!SaveHostManifest(manifest, installed, ids)) {
      ShowMessage(silent, L"无法写入浏览器通信清单。", MB_ICONERROR);
      return 5;
    }
    for (size_t i = 0; i < std::size(kBrowsers); ++i) {
      if (browsersFound[i] && !RegisterHost(kBrowsers[i], manifest)) {
        ShowMessage(silent, L"无法注册浏览器通信。", MB_ICONERROR);
        return 6;
      }
    }
  }

  if (extensions.empty() || outdated) {
    if (!silent) {
      const std::wstring status = extensions.empty()
          ? L"未检测到已安装的 PopTab 扩展。"
          : L"检测到了旧版 PopTab，但它还不支持 Windows 原生通信。";
      const std::wstring message =
          status + L"\n\n新版扩展已解压至：\n" +
          extensionFolder.wstring() +
          L"\n\n请在浏览器扩展管理页面启用开发者模式，"
          L"选择“加载已解压的扩展程序”，指定上述文件夹。"
          L"\n完成后再次双击本 EXE，即可自动绑定通信。"
          L"\n\n是否现在打开扩展所在文件夹？";
      if (MessageBoxW(nullptr, message.c_str(), L"PopTab Companion",
                      MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES) {
        ShellExecuteW(nullptr, L"open", extensionFolder.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
      }
    }
    return extensions.empty() ? 7 : 8;
  }

  const std::wstring message =
      L"安装完成！\n\n已自动识别 " + std::to_wstring(extensions.size()) +
      L" 个 PopTab 扩展，并配置原生置顶通信。"
      L"\n无需输入扩展 ID，也不需要管理员权限。"
      L"\n\n如果刚更新了扩展，请在浏览器扩展页重新加载一次。"
      L"\n打开 PopTab 弹窗，点击左上角图钉即可置顶。";
  ShowMessage(silent, message, MB_ICONINFORMATION);
  return 0;
}
