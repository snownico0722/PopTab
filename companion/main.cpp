// PopTab Companion: optional Windows-only native messaging host.
// No browser injection, no polling of webpage content, no third-party UI framework.
#include <windows.h>
#include <objidl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <tlhelp32.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <climits>
#include <cwchar>
#include <iterator>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t kDispatcherClass[] = L"PopTabPinDispatcher";
constexpr wchar_t kButtonClass[] = L"PopTabPinButton";
constexpr UINT kCommandMessage = WM_APP + 1;
constexpr UINT kShutdownMessage = WM_APP + 2;
constexpr UINT_PTR kTimerId = 1;
constexpr size_t kMaxMessageBytes = 65536;
constexpr size_t kMaxTrackedWindows = 64;
constexpr ULONGLONG kRetireMs = 15000;

struct Bounds {
  int id = 0;
  int left = 0;
  int top = 0;
  int width = 0;
  int height = 0;
};

struct Command {
  enum class Kind { Sync, Transfer } kind;
  std::vector<Bounds> windows;
  int from = 0;
  int to = 0;
};

struct Entry {
  Bounds expected;
  HWND target = nullptr;
  HWND button = nullptr;
  int buttonSize = 0;
  int x = 0;
  int y = 0;
  bool pinned = false;
  bool hovered = false;
  std::optional<bool> inheritedPin;
  ULONGLONG firstSeen = 0;
};

struct Retired {
  bool pinned = false;
  ULONGLONG expires = 0;
};

HWND g_dispatcher = nullptr;
HANDLE g_input = INVALID_HANDLE_VALUE;
HANDLE g_output = INVALID_HANDLE_VALUE;
DWORD g_browserPid = 0;
std::map<int, Entry> g_entries;
std::map<int, Retired> g_retired;
std::map<int, bool> g_pendingTransfers;
std::vector<HWINEVENTHOOK> g_hooks;
ULONG_PTR g_gdiplusToken = 0;

bool ReadAll(void* buffer, DWORD bytes) {
  auto* dest = static_cast<BYTE*>(buffer);
  while (bytes) {
    DWORD read = 0;
    if (!ReadFile(g_input, dest, bytes, &read, nullptr) || !read) return false;
    dest += read;
    bytes -= read;
  }
  return true;
}

bool WriteAll(const void* buffer, DWORD bytes) {
  auto* src = static_cast<const BYTE*>(buffer);
  while (bytes) {
    DWORD written = 0;
    if (!WriteFile(g_output, src, bytes, &written, nullptr) || !written) {
      return false;
    }
    src += written;
    bytes -= written;
  }
  return true;
}

void Send(const std::string& json) {
  if (json.size() > kMaxMessageBytes) return;
  const uint32_t size = static_cast<uint32_t>(json.size());
  const BYTE prefix[4] = {
      static_cast<BYTE>(size), static_cast<BYTE>(size >> 8),
      static_cast<BYTE>(size >> 16), static_cast<BYTE>(size >> 24)};
  WriteAll(prefix, 4);
  WriteAll(json.data(), size);
}

void SendState(const Entry& entry) {
  Send("{\"type\":\"state\",\"id\":" + std::to_string(entry.expected.id) +
       ",\"pinned\":" + (entry.pinned ? "true}" : "false}"));
}

// The extension sends only numeric fields and an ASCII command. Parse those
// exact values without admitting native window handles supplied by a page.
std::optional<std::string> StringField(
    const std::string& text, const char* name, size_t begin, size_t end) {
  const std::string key = std::string("\"") + name + "\"";
  size_t pos = text.find(key, begin);
  if (pos == std::string::npos || pos + key.size() >= end) return {};
  pos += key.size();
  while (pos < end && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
  if (pos >= end || text[pos++] != ':') return {};
  while (pos < end && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
  if (pos >= end || text[pos++] != '"') return {};
  const size_t finish = text.find('"', pos);
  if (finish == std::string::npos || finish >= end) return {};
  return text.substr(pos, finish - pos);
}

std::optional<int> IntField(
    const std::string& text, const char* name, size_t begin, size_t end) {
  const std::string key = std::string("\"") + name + "\"";
  size_t pos = text.find(key, begin);
  if (pos == std::string::npos || pos + key.size() >= end) return {};
  pos += key.size();
  while (pos < end && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
  if (pos >= end || text[pos++] != ':') return {};
  while (pos < end && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
  const char* start = text.data() + pos;
  int value = 0;
  const auto parsed = std::from_chars(start, text.data() + end, value);
  if (parsed.ec != std::errc{} || parsed.ptr == start) return {};
  return value;
}

std::unique_ptr<Command> Parse(const std::string& json) {
  const auto type = StringField(json, "type", 0, json.size());
  if (!type) return {};
  auto command = std::make_unique<Command>();

  if (*type == "transfer") {
    command->kind = Command::Kind::Transfer;
    const auto from = IntField(json, "from", 0, json.size());
    const auto to = IntField(json, "to", 0, json.size());
    if (!from || !to || *from <= 0 || *to <= 0 || *from == *to) return {};
    command->from = *from;
    command->to = *to;
    return command;
  }

  if (*type != "sync") return {};
  command->kind = Command::Kind::Sync;
  size_t start = json.find("\"windows\"");
  if (start == std::string::npos) return {};
  start = json.find('[', start + 9);
  if (start == std::string::npos) return {};
  size_t end = json.find(']', start + 1);
  if (end == std::string::npos) return {};

  for (size_t at = start + 1; at < end;) {
    const size_t open = json.find('{', at);
    if (open == std::string::npos || open >= end) break;
    const size_t close = json.find('}', open + 1);
    if (close == std::string::npos || close >= end) return {};
    auto id = IntField(json, "id", open, close);
    auto left = IntField(json, "left", open, close);
    auto top = IntField(json, "top", open, close);
    auto width = IntField(json, "width", open, close);
    auto height = IntField(json, "height", open, close);
    if (!id || !left || !top || !width || !height || *id <= 0 ||
        *width <= 0 || *height <= 0 || *width > 32768 || *height > 32768 ||
        std::abs(static_cast<int64_t>(*left)) > 1000000 ||
        std::abs(static_cast<int64_t>(*top)) > 1000000 ||
        command->windows.size() >= kMaxTrackedWindows) {
      return {};
    }
    command->windows.push_back({*id, *left, *top, *width, *height});
    at = close + 1;
  }
  return command;
}

DWORD ParentProcessId() {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD parent = 0;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == GetCurrentProcessId()) {
        parent = entry.th32ParentProcessID;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return parent;
}

bool IsBrowserWindow(HWND window) {
  if (!g_browserPid || !IsWindow(window) || !IsWindowVisible(window) ||
      IsIconic(window) || GetAncestor(window, GA_ROOT) != window) {
    return false;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != g_browserPid) return false;
  wchar_t className[80]{};
  return GetClassNameW(window, className, 80) &&
         wcscmp(className, L"Chrome_WidgetWin_1") == 0;
}

RECT VisibleBounds(HWND window) {
  RECT bounds{};
  if (FAILED(DwmGetWindowAttribute(
          window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
    GetWindowRect(window, &bounds);
  }
  return bounds;
}

struct Match {
  HWND window = nullptr;
  int score = 0;
};

struct MatchContext {
  Bounds expected;
  HWND foreground = nullptr;
  std::vector<Match> matches;
};

BOOL CALLBACK CollectMatches(HWND window, LPARAM parameter) {
  auto& context = *reinterpret_cast<MatchContext*>(parameter);
  if (!IsBrowserWindow(window)) return TRUE;
  const RECT visible = VisibleBounds(window);
  RECT full{};
  GetWindowRect(window, &full);
  const UINT dpi = GetDpiForWindow(window);
  const int tolerance = std::max(22, static_cast<int>(dpi * 22 / 96));
  int best = INT_MAX;
  for (const RECT& bounds : {visible, full}) {
    const int differences[] = {
        std::abs(bounds.left - context.expected.left),
        std::abs(bounds.top - context.expected.top),
        std::abs((bounds.right - bounds.left) - context.expected.width),
        std::abs((bounds.bottom - bounds.top) - context.expected.height)};
    if (*std::max_element(std::begin(differences), std::end(differences)) >
        tolerance) {
      continue;
    }
    best = std::min(best, differences[0] + differences[1] +
                              differences[2] + differences[3]);
  }
  if (best != INT_MAX) context.matches.push_back({window, best});
  return TRUE;
}

HWND FindTarget(const Bounds& expected) {
  MatchContext matches{expected, GetForegroundWindow(), {}};
  EnumWindows(CollectMatches, reinterpret_cast<LPARAM>(&matches));
  if (matches.matches.empty()) return nullptr;
  if (matches.matches.size() == 1) return matches.matches.front().window;

  // A freshly created PopTab popup is focused. The original browser may
  // have exactly the same bounds, so geometry alone must not pick it.
  for (const auto& match : matches.matches) {
    if (match.window == matches.foreground) return match.window;
  }
  std::sort(matches.matches.begin(), matches.matches.end(),
            [](const Match& a, const Match& b) { return a.score < b.score; });
  if (matches.matches[1].score - matches.matches[0].score > 20) {
    return matches.matches[0].window;
  }
  return nullptr;  // ambiguous: fail closed instead of pinning another window
}

void DrawButton(Entry& entry) {
  if (!entry.button || !IsWindow(entry.button) || entry.buttonSize <= 0) return;
  const int size = entry.buttonSize;
  HDC screen = GetDC(nullptr);
  if (!screen) return;
  HDC memory = CreateCompatibleDC(screen);
  if (!memory) {
    ReleaseDC(nullptr, screen);
    return;
  }
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = size;
  info.bmiHeader.biHeight = -size;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels,
                                nullptr, 0);
  if (dib && pixels) {
    HGDIOBJ oldBitmap = SelectObject(memory, dib);
    {
    Gdiplus::Bitmap image(size, size, size * 4,
                          PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
    Gdiplus::Graphics graphics(&image);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    graphics.ScaleTransform(static_cast<Gdiplus::REAL>(size) / 28.0f,
                            static_cast<Gdiplus::REAL>(size) / 28.0f);

    Gdiplus::GraphicsPath background;
    background.AddArc(0.6f, 0.6f, 6.0f, 6.0f, 180.0f, 90.0f);
    background.AddArc(21.4f, 0.6f, 6.0f, 6.0f, 270.0f, 90.0f);
    background.AddArc(21.4f, 21.4f, 6.0f, 6.0f, 0.0f, 90.0f);
    background.AddArc(0.6f, 21.4f, 6.0f, 6.0f, 90.0f, 90.0f);
    background.CloseFigure();
    const Gdiplus::Color fill = entry.pinned
        ? Gdiplus::Color(225, 37, 105, 197)
        : entry.hovered ? Gdiplus::Color(195, 55, 60, 70)
                        : Gdiplus::Color(135, 48, 52, 60);
    Gdiplus::SolidBrush brush(fill);
    graphics.FillPath(&brush, &background);

    const Gdiplus::Color icon = entry.pinned
        ? Gdiplus::Color(255, 255, 255, 255)
        : Gdiplus::Color(235, 244, 246, 250);
    Gdiplus::Pen pen(icon, 1.8f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    const Gdiplus::PointF head[] = {
        {10.0f, 7.2f}, {18.0f, 7.2f}, {17.0f, 11.5f},
        {20.0f, 15.0f}, {8.0f, 15.0f}, {11.0f, 11.5f}};
    if (entry.pinned) {
      Gdiplus::SolidBrush solid(icon);
      graphics.FillPolygon(&solid, head, 6);
    } else {
      graphics.DrawPolygon(&pen, head, 6);
    }
    graphics.DrawLine(&pen, 14.0f, 15.0f, 12.4f, 22.0f);
    graphics.Flush(Gdiplus::FlushIntentionSync);
    }

    POINT destination{entry.x, entry.y};
    SIZE imageSize{size, size};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(entry.button, screen, &destination, &imageSize,
                        memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, oldBitmap);
    DeleteObject(dib);
  } else if (dib) {
    DeleteObject(dib);
  }
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
}

void RefreshPosition(Entry& entry) {
  if (!entry.target || !IsWindow(entry.target) ||
      !entry.button || !IsWindow(entry.button)) return;
  if (!IsWindowVisible(entry.target) || IsIconic(entry.target) ||
      !(GetWindowLongPtrW(entry.target, GWL_STYLE) & WS_CAPTION)) {
    ShowWindow(entry.button, SW_HIDE);
    return;
  }

  const UINT dpi = GetDpiForWindow(entry.target);
  const int size = std::max(28, MulDiv(28, dpi, 96));
  const RECT visible = VisibleBounds(entry.target);
  const int titleHeight = std::max(30, MulDiv(32, dpi, 96));
  // Leave the browser/system icon and system menu untouched.
  const int x = visible.left + MulDiv(42, dpi, 96);
  const int y = visible.top + (titleHeight - size) / 2;
  if (visible.right - x < size + MulDiv(80, dpi, 96)) {
    ShowWindow(entry.button, SW_HIDE);
    return;
  }

  const bool changed = entry.x != x || entry.y != y ||
                       entry.buttonSize != size;
  const bool resize = entry.buttonSize != size;
  entry.x = x;
  entry.y = y;
  entry.buttonSize = size;
  if (changed) {
    SetWindowPos(entry.button, nullptr, x, y, size, size,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
  if (resize) DrawButton(entry);
  if (!IsWindowVisible(entry.button)) ShowWindow(entry.button, SW_SHOWNOACTIVATE);

  const bool pinned = (GetWindowLongPtrW(entry.target, GWL_EXSTYLE) &
                       WS_EX_TOPMOST) != 0;
  if (entry.pinned != pinned) {
    entry.pinned = pinned;
    DrawButton(entry);
    SendState(entry);
  }
}

void SetPinned(Entry& entry, bool pinned) {
  if (!entry.target || !IsWindow(entry.target)) return;
  const HWND after = pinned ? HWND_TOPMOST : HWND_NOTOPMOST;
  if (SetWindowPos(entry.target, after, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
    entry.pinned = (GetWindowLongPtrW(entry.target, GWL_EXSTYLE) &
                    WS_EX_TOPMOST) != 0;
    DrawButton(entry);
    SendState(entry);
  }
}

LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  auto* entry = reinterpret_cast<Entry*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return TRUE;
  }
  if (!entry) return DefWindowProcW(window, message, wp, lp);
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    case WM_MOUSEMOVE: {
      if (!entry->hovered) {
        entry->hovered = true;
        DrawButton(*entry);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      entry->hovered = false;
      DrawButton(*entry);
      return 0;
    case WM_LBUTTONDOWN:
      SetCapture(window);
      return 0;
    case WM_LBUTTONUP:
      if (GetCapture() == window) {
        ReleaseCapture();
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT buttonRect{};
        GetClientRect(window, &buttonRect);
        if (PtInRect(&buttonRect, point)) SetPinned(*entry, !entry->pinned);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProcW(window, message, wp, lp);
}

void Retire(int id) {
  auto found = g_entries.find(id);
  if (found == g_entries.end()) return;
  Entry& entry = found->second;
  // Preserve the old state even when Chromium destroyed the HWND first.
  g_retired[id] = {entry.pinned, GetTickCount64() + kRetireMs};
  if (entry.button && IsWindow(entry.button)) {
    SetWindowLongPtrW(entry.button, GWLP_USERDATA, 0);
    DestroyWindow(entry.button);
  }
  g_entries.erase(found);
}

void TryBind(Entry& entry) {
  if (entry.target || GetTickCount64() - entry.firstSeen > 8000) return;
  HWND target = FindTarget(entry.expected);
  if (!target) return;
  entry.target = target;
  entry.pinned = (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
  entry.button = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      kButtonClass, L"", WS_POPUP, 0, 0, 28, 28, target, nullptr,
      GetModuleHandleW(nullptr), &entry);
  if (!entry.button) {
    entry.target = nullptr;
    return;
  }
  if (entry.inheritedPin.has_value()) {
    SetPinned(entry, *entry.inheritedPin);
    entry.inheritedPin.reset();
  }
  RefreshPosition(entry);
  DrawButton(entry);
  SendState(entry);
}

void OnSync(const std::vector<Bounds>& windows) {
  std::map<int, Bounds> incoming;
  for (const auto& window : windows) incoming[window.id] = window;

  std::vector<int> toRemove;
  for (const auto& [id, entry] : g_entries) {
    if (!incoming.count(id)) toRemove.push_back(id);
  }
  for (int id : toRemove) Retire(id);

  for (const auto& [id, expected] : incoming) {
    auto [it, inserted] = g_entries.try_emplace(id);
    Entry& entry = it->second;
    entry.expected = expected;
    if (inserted) entry.firstSeen = GetTickCount64();
    if (const auto carry = g_pendingTransfers.find(id);
        carry != g_pendingTransfers.end()) {
      entry.inheritedPin = carry->second;
      g_pendingTransfers.erase(carry);
    }
    if (entry.target && !IsWindow(entry.target)) {
      if (entry.button && IsWindow(entry.button)) {
        SetWindowLongPtrW(entry.button, GWLP_USERDATA, 0);
        DestroyWindow(entry.button);
      }
      entry.button = nullptr;
      entry.target = nullptr;
      entry.firstSeen = GetTickCount64();
    }
    if (!entry.target) TryBind(entry);
    else RefreshPosition(entry);
  }
}

void OnTransfer(int from, int to) {
  std::optional<bool> pinned;
  const auto source = g_entries.find(from);
  if (source != g_entries.end()) pinned = source->second.pinned;
  const auto retired = g_retired.find(from);
  if (!pinned && retired != g_retired.end() &&
      retired->second.expires > GetTickCount64()) {
    pinned = retired->second.pinned;
  }
  if (!pinned.has_value()) return;
  const auto dest = g_entries.find(to);
  if (dest == g_entries.end()) {
    g_pendingTransfers[to] = *pinned;
    return;
  }
  if (dest->second.target) SetPinned(dest->second, *pinned);
  else dest->second.inheritedPin = *pinned;
}

void CALLBACK OnWindowEvent(HWINEVENTHOOK, DWORD event, HWND window,
                            LONG objectId, LONG, DWORD, DWORD) {
  if (objectId != OBJID_WINDOW) return;
  if (event == EVENT_SYSTEM_FOREGROUND) {
    for (auto& [id, entry] : g_entries) {
      if (!entry.target) TryBind(entry);
    }
    return;
  }
  for (auto& [id, entry] : g_entries) {
    if (entry.target == window) {
      RefreshPosition(entry);
      break;
    }
  }
}

LRESULT CALLBACK DispatcherProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  switch (message) {
    case kCommandMessage: {
      std::unique_ptr<Command> command(reinterpret_cast<Command*>(lp));
      if (command->kind == Command::Kind::Sync) OnSync(command->windows);
      else OnTransfer(command->from, command->to);
      return 0;
    }
    case WM_TIMER: {
      for (auto& [id, entry] : g_entries) {
        if (entry.target && !IsWindow(entry.target)) {
          entry.target = nullptr;
          entry.button = nullptr;  // owner destroyed the owned button
          entry.firstSeen = GetTickCount64();
        }
        if (!entry.target) TryBind(entry);
        else RefreshPosition(entry);
      }
      for (auto it = g_retired.begin(); it != g_retired.end();) {
        if (it->second.expires < GetTickCount64()) it = g_retired.erase(it);
        else ++it;
      }
      return 0;
    }
    case kShutdownMessage:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wp, lp);
}

void ReadCommands(HWND receiver) {
  while (true) {
    BYTE prefix[4]{};
    if (!ReadAll(prefix, sizeof(prefix))) break;
    const uint32_t length = static_cast<uint32_t>(prefix[0]) |
        (static_cast<uint32_t>(prefix[1]) << 8) |
        (static_cast<uint32_t>(prefix[2]) << 16) |
        (static_cast<uint32_t>(prefix[3]) << 24);
    if (length == 0 || length > kMaxMessageBytes) break;
    std::string json(length, '\0');
    if (!ReadAll(json.data(), length)) break;
    auto command = Parse(json);
    if (!command) continue;
    Command* raw = command.release();
    if (!PostMessageW(receiver, kCommandMessage, 0,
                      reinterpret_cast<LPARAM>(raw))) delete raw;
  }
  PostMessageW(receiver, kShutdownMessage, 0, 0);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  g_input = GetStdHandle(STD_INPUT_HANDLE);
  g_output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!g_input || !g_output || g_input == INVALID_HANDLE_VALUE ||
      g_output == INVALID_HANDLE_VALUE) return 1;
  g_browserPid = ParentProcessId();

  Gdiplus::GdiplusStartupInput startup;
  if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &startup, nullptr) !=
      Gdiplus::Ok) return 1;

  WNDCLASSW dispatcher{};
  dispatcher.lpfnWndProc = DispatcherProc;
  dispatcher.hInstance = instance;
  dispatcher.lpszClassName = kDispatcherClass;
  WNDCLASSW button{};
  button.lpfnWndProc = ButtonProc;
  button.hInstance = instance;
  button.lpszClassName = kButtonClass;
  button.hCursor = LoadCursorW(nullptr, IDC_HAND);
  if (!RegisterClassW(&dispatcher) || !RegisterClassW(&button)) {
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 1;
  }
  g_dispatcher = CreateWindowExW(0, kDispatcherClass, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, instance, nullptr);
  if (!g_dispatcher) {
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 1;
  }
  for (const auto [first, last] : {
           std::pair<DWORD, DWORD>{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
           {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
           {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
           {EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY}}) {
    const HWINEVENTHOOK hook = SetWinEventHook(
        first, last, nullptr, OnWindowEvent, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (hook) g_hooks.push_back(hook);
  }
  SetTimer(g_dispatcher, kTimerId, 500, nullptr);
  Send("{\"type\":\"ready\"}");
  std::thread reader(ReadCommands, g_dispatcher);
  reader.detach();

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  for (const auto& [id, entry] : g_entries) {
    if (entry.button && IsWindow(entry.button)) {
      SetWindowLongPtrW(entry.button, GWLP_USERDATA, 0);
      DestroyWindow(entry.button);
    }
  }
  for (auto hook : g_hooks) UnhookWinEvent(hook);
  Gdiplus::GdiplusShutdown(g_gdiplusToken);
  return 0;
}
