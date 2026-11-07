// Win32 titlebar pin. The extension confirms the browser window ID;
// this host confirms that the foreground HWND did not change meanwhile.
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <tlhelp32.h>
#include "setup.h"
#include "tray.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cwchar>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t kDispatcherClass[] = L"PopTabPinDispatcher";
constexpr wchar_t kButtonClass[] = L"PopTabPinButton";
constexpr wchar_t kOwnerProperty[] = L"PopTab.TitlePin.Owner";
constexpr UINT kCommandMessage = WM_APP + 1;
constexpr UINT kShutdownMessage = WM_APP + 2;
constexpr size_t kMaxMessageBytes = 65536;
constexpr ULONGLONG kRetireMs = 15000;
struct Command {
  std::string type;
  std::vector<int> windows;
  int from = 0, to = 0, request = 0, id = 0;
};
struct Entry {
  int id = 0;
  HWND target = nullptr, button = nullptr;
  int size = 0, x = 0, y = 0;
  bool pinned = false, hovered = false, originalPin = false, changedPin = false;
  std::optional<bool> inheritedPin;
};
struct Retired { bool pinned; ULONGLONG expires; };
HWND g_dispatcher = nullptr, g_focusWindow = nullptr;
HANDLE g_input = INVALID_HANDLE_VALUE, g_output = INVALID_HANDLE_VALUE;
DWORD g_browserPid = 0;
int g_focusRequest = 0;
std::map<int, Entry> g_entries;
std::map<int, Retired> g_retired, g_pendingTransfers;
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
    if (!WriteFile(g_output, src, bytes, &written, nullptr) || !written) return false;
    src += written;
    bytes -= written;
  }
  return true;
}
void Send(const std::string& json) {
  const uint32_t size = static_cast<uint32_t>(json.size());
  const BYTE prefix[4] = {static_cast<BYTE>(size), static_cast<BYTE>(size >> 8),
                         static_cast<BYTE>(size >> 16), static_cast<BYTE>(size >> 24)};
  if (!WriteAll(prefix, 4) || !WriteAll(json.data(), size))
    PostMessageW(g_dispatcher, kShutdownMessage, 0, 0);
}
// Messages are emitted only by our extension, with ASCII keys and integer IDs.
std::optional<int> IntField(const std::string& text, const char* key,
                            size_t begin = 0, size_t end = std::string::npos) {
  end = std::min(end, text.size());
  size_t pos = text.find(std::string("\"") + key + "\"", begin);
  if (pos == std::string::npos || pos >= end) return {};
  pos = text.find(':', pos);
  if (pos == std::string::npos || ++pos >= end) return {};
  while (pos < end && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
  int value = 0;
  auto parsed = std::from_chars(text.data() + pos, text.data() + end, value);
  if (parsed.ec != std::errc{}) return {};
  return value;
}
std::unique_ptr<Command> Parse(const std::string& json) {
  size_t pos = json.find("\"type\"");
  if (pos == std::string::npos) return {};
  pos = json.find(':', pos);
  if (pos == std::string::npos) return {};
  pos = json.find('"', pos);
  if (pos == std::string::npos) return {};
  const size_t end = json.find('"', pos + 1);
  if (end == std::string::npos) return {};
  auto command = std::make_unique<Command>();
  command->type = json.substr(pos + 1, end - pos - 1);
  if (command->type == "shutdown") return command;
  if (command->type == "focus") {
    command->request = IntField(json, "request").value_or(0);
    command->id = IntField(json, "id").value_or(0);
    return command;
  }
  if (command->type == "transfer") {
    command->from = IntField(json, "from").value_or(0);
    command->to = IntField(json, "to").value_or(0);
    if (command->from <= 0 || command->to <= 0 || command->from == command->to) return {};
    return command;
  }
  if (command->type != "sync") return {};
  pos = json.find("\"windows\"");
  if (pos == std::string::npos) return {};
  const size_t finish = json.find(']', pos);
  if (finish == std::string::npos) return {};
  while ((pos = json.find('{', pos)) != std::string::npos && pos < finish) {
    const size_t close = json.find('}', pos);
    if (close == std::string::npos || close > finish) return {};
    auto id = IntField(json, "id", pos, close);
    if (!id || *id <= 0) return {};
    command->windows.push_back(*id);
    pos = close + 1;
  }
  return command;
}
DWORD ParentProcessId() { // Test harness only; real Chromium can launch via cmd.exe.
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD parent = 0;
  if (Process32FirstW(snapshot, &entry)) do {
    if (entry.th32ProcessID == GetCurrentProcessId()) {
      parent = entry.th32ParentProcessID;
      break;
    }
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return parent;
}
bool IsBrowserWindow(HWND window) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  wchar_t name[80]{};
  return g_browserPid && pid == g_browserPid && GetAncestor(window, GA_ROOT) == window &&
         GetClassNameW(window, name, 80) && wcscmp(name, L"Chrome_WidgetWin_1") == 0;
}
bool OwnsTarget(const Entry& entry) {
  return IsBrowserWindow(entry.target) &&
         GetPropW(entry.target, kOwnerProperty) == g_dispatcher;
}
bool IsPinned(HWND window) {
  return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}
RECT VisibleBounds(HWND window) {
  RECT rect{};
  if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
    GetWindowRect(window, &rect);
  return rect;
}
void SendState(const Entry& entry) {
  Send("{\"type\":\"state\",\"id\":" + std::to_string(entry.id) +
       ",\"pinned\":" + (entry.pinned ? "true}" : "false}"));
}
void DrawButton(Entry& entry) {
  if (!IsWindow(entry.button) || entry.size <= 0) return;
  HDC screen = GetDC(nullptr);
  if (!screen) return;
  HDC memory = CreateCompatibleDC(screen);
  if (!memory) { ReleaseDC(nullptr, screen); return; }
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = entry.size;
  info.bmiHeader.biHeight = -entry.size;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (dib && pixels) {
    HGDIOBJ old = SelectObject(memory, dib);
    {
      Gdiplus::Bitmap image(entry.size, entry.size, entry.size * 4,
                            PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
      Gdiplus::Graphics graphics(&image);
      graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
      graphics.ScaleTransform(entry.size / 28.0f, entry.size / 28.0f);
      Gdiplus::GraphicsPath background;
      background.AddArc(0.6f, 0.6f, 6.0f, 6.0f, 180.0f, 90.0f);
      background.AddArc(21.4f, 0.6f, 6.0f, 6.0f, 270.0f, 90.0f);
      background.AddArc(21.4f, 21.4f, 6.0f, 6.0f, 0.0f, 90.0f);
      background.AddArc(0.6f, 21.4f, 6.0f, 6.0f, 90.0f, 90.0f);
      background.CloseFigure();
      Gdiplus::SolidBrush brush(entry.pinned ? Gdiplus::Color(225, 37, 105, 197) :
          entry.hovered ? Gdiplus::Color(195, 55, 60, 70) : Gdiplus::Color(135, 48, 52, 60));
      graphics.FillPath(&brush, &background);
      Gdiplus::Color icon(255, 244, 246, 250);
      Gdiplus::Pen pen(icon, 1.8f);
      pen.SetLineJoin(Gdiplus::LineJoinRound);
      const Gdiplus::PointF head[] = {{10.0f, 7.2f}, {18.0f, 7.2f}, {17.0f, 11.5f},
                                    {20.0f, 15.0f}, {8.0f, 15.0f}, {11.0f, 11.5f}};
      if (entry.pinned) {
        Gdiplus::SolidBrush solid(icon);
        graphics.FillPolygon(&solid, head, 6);
      } else graphics.DrawPolygon(&pen, head, 6);
      graphics.DrawLine(&pen, 14.0f, 15.0f, 12.4f, 22.0f);
      graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    POINT dest{entry.x, entry.y}, source{};
    SIZE size{entry.size, entry.size};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(entry.button, screen, &dest, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, old);
  }
  if (dib) DeleteObject(dib);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
}
void RefreshPosition(Entry& entry) {
  if (!OwnsTarget(entry) || !IsWindow(entry.button)) return;
  DWORD cloaked = 0;
  DwmGetWindowAttribute(entry.target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  if (!IsWindowVisible(entry.target) || IsIconic(entry.target) || cloaked ||
      (GetWindowLongPtrW(entry.target, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) {
    ShowWindow(entry.button, SW_HIDE);
    return;
  }
  const UINT dpi = GetDpiForWindow(entry.target);
  const RECT bounds = VisibleBounds(entry.target);
  // Anchor in the actual Windows caption area, immediately after the
  // system-menu icon instead of using a fixed offset from the page.
  const int size = MulDiv(26, dpi, 96);
  const int captionHeight = std::max(MulDiv(30, dpi, 96),
      GetSystemMetricsForDpi(SM_CYCAPTION, dpi) + GetSystemMetricsForDpi(SM_CYFRAME, dpi));
  const int x = bounds.left + MulDiv(30, dpi, 96);
  const int y = bounds.top + std::max(0, (captionHeight - size) / 2);
  if (bounds.right - x < size + MulDiv(80, dpi, 96)) {
    ShowWindow(entry.button, SW_HIDE);
    return;
  }
  const bool redraw = entry.size != size;
  if (entry.x != x || entry.y != y || redraw) {
    entry.x = x; entry.y = y; entry.size = size;
    SetWindowPos(entry.button, nullptr, x, y, size, size,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
  if (redraw) DrawButton(entry);
  if (!IsWindowVisible(entry.button)) ShowWindow(entry.button, SW_SHOWNOACTIVATE);
  if (entry.pinned != IsPinned(entry.target)) {
    entry.pinned = IsPinned(entry.target);
    entry.changedPin = false; // An external tool changed it; don't undo that on exit.
    DrawButton(entry);
    SendState(entry);
  }
}
void SetPinned(Entry& entry, bool pinned) {
  if (!OwnsTarget(entry)) return;
  if (SetWindowPos(entry.target, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
    entry.pinned = IsPinned(entry.target);
    entry.changedPin = entry.pinned != entry.originalPin;
    DrawButton(entry);
    // Changing the owner to TOPMOST can leave a layered owned popup behind
    // it until the next activation. Place the button in the same z-order band
    // immediately: a second click must not require switching focus.
    if (IsWindow(entry.button))
      SetWindowPos(entry.button, entry.pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                   0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SendState(entry);
  }
}
LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  auto* entry = reinterpret_cast<Entry*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA,
      reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    return TRUE;
  }
  if (!entry) return DefWindowProcW(window, message, wp, lp);
  switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
    case WM_MOUSEMOVE:
      if (!entry->hovered) {
        entry->hovered = true;
        DrawButton(*entry);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
      }
      return 0;
    case WM_MOUSELEAVE: entry->hovered = false; DrawButton(*entry); return 0;
    case WM_LBUTTONDOWN: SetCapture(window); return 0;
    case WM_LBUTTONUP:
      if (GetCapture() == window) {
        ReleaseCapture();
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT rect{};
        GetClientRect(window, &rect);
        if (PtInRect(&rect, point)) SetPinned(*entry, !IsPinned(entry->target));
      }
      return 0;
    case WM_ERASEBKGND: return 1;
  }
  return DefWindowProcW(window, message, wp, lp);
}
void Release(Entry& entry) {
  if (OwnsTarget(entry)) {
    if (entry.changedPin && IsPinned(entry.target) == entry.pinned)
      SetWindowPos(entry.target, entry.originalPin ? HWND_TOPMOST : HWND_NOTOPMOST,
                   0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RemovePropW(entry.target, kOwnerProperty);
  }
  if (IsWindow(entry.button)) {
    SetWindowLongPtrW(entry.button, GWLP_USERDATA, 0);
    DestroyWindow(entry.button);
  }
  entry.target = entry.button = nullptr;
}
void RequestFocus() {
  g_focusWindow = GetForegroundWindow();
  ++g_focusRequest; // Invalidates any reply about the preceding foreground window.
  if (!g_entries.empty() && IsBrowserWindow(g_focusWindow))
    Send("{\"type\":\"focus\",\"request\":" + std::to_string(g_focusRequest) + "}");
}
void BindFocus(const Command& command) {
  auto found = g_entries.find(command.id);
  if (command.request != g_focusRequest || found == g_entries.end() ||
      g_focusWindow != GetForegroundWindow() || !IsBrowserWindow(g_focusWindow) ||
      IsIconic(g_focusWindow)) return;
  Entry& entry = found->second;
  if (OwnsTarget(entry)) return;
  // Never attach two buttons to the same HWND, including another host instance.
  HWND owner = static_cast<HWND>(GetPropW(g_focusWindow, kOwnerProperty));
  if (IsWindow(owner)) return;
  if (!SetPropW(g_focusWindow, kOwnerProperty, g_dispatcher)) return;
  entry.target = g_focusWindow;
  entry.originalPin = entry.pinned = IsPinned(entry.target);
  entry.changedPin = false;
  entry.button = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      kButtonClass, L"", WS_POPUP, 0, 0, 28, 28, entry.target, nullptr,
      GetModuleHandleW(nullptr), &entry);
  if (!entry.button) { Release(entry); return; }
  if (entry.inheritedPin) SetPinned(entry, *entry.inheritedPin);
  entry.inheritedPin.reset();
  RefreshPosition(entry);
  DrawButton(entry);
  SendState(entry);
}
void OnSync(const std::vector<int>& ids) {
  for (auto it = g_entries.begin(); it != g_entries.end();) {
    if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) {
      g_retired[it->first] = {it->second.pinned, GetTickCount64() + kRetireMs};
      Release(it->second);
      it = g_entries.erase(it);
    } else ++it;
  }
  for (int id : ids) {
    Entry& entry = g_entries[id];
    entry.id = id;
    if (auto carry = g_pendingTransfers.find(id); carry != g_pendingTransfers.end()) {
      if (carry->second.expires > GetTickCount64()) entry.inheritedPin = carry->second.pinned;
      g_pendingTransfers.erase(carry);
    }
  }
  RequestFocus();
}
void OnTransfer(int from, int to) {
  std::optional<bool> pinned;
  if (auto source = g_entries.find(from); source != g_entries.end()) pinned = source->second.pinned;
  else if (auto retired = g_retired.find(from); retired != g_retired.end() &&
           retired->second.expires > GetTickCount64()) pinned = retired->second.pinned;
  if (!pinned) return;
  auto dest = g_entries.find(to);
  if (dest == g_entries.end()) g_pendingTransfers[to] = {*pinned, GetTickCount64() + kRetireMs};
  else if (OwnsTarget(dest->second)) SetPinned(dest->second, *pinned);
  else dest->second.inheritedPin = *pinned;
}
void CALLBACK OnWindowEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG, DWORD, DWORD) {
  if (event == EVENT_SYSTEM_FOREGROUND) { RequestFocus(); return; }
  if (object != OBJID_WINDOW) return;
  for (auto& [id, entry] : g_entries) if (entry.target == window) {
    if (event == EVENT_OBJECT_DESTROY) Release(entry);
    else RefreshPosition(entry);
    break;
  }
}
LRESULT CALLBACK DispatcherProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  switch (message) {
    case kCommandMessage: {
      std::unique_ptr<Command> command(reinterpret_cast<Command*>(lp));
      if (command->type == "sync") OnSync(command->windows);
      else if (command->type == "focus") BindFocus(*command);
      else if (command->type == "transfer") OnTransfer(command->from, command->to);
      else if (command->type == "shutdown") DestroyWindow(window);
      return 0;
    }
    case WM_TIMER:
      for (auto& [id, entry] : g_entries) RefreshPosition(entry);
      for (auto* cache : {&g_retired, &g_pendingTransfers})
        for (auto it = cache->begin(); it != cache->end();)
          if (it->second.expires < GetTickCount64()) it = cache->erase(it); else ++it;
      return 0;
    case kShutdownMessage: DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(window, message, wp, lp);
}
void ReadCommands(HWND receiver) {
  while (true) {
    uint32_t size = 0; // Native Messaging uses host byte order; Windows is little-endian.
    if (!ReadAll(&size, sizeof(size)) || !size || size > kMaxMessageBytes) break;
    std::string json(size, '\0');
    if (!ReadAll(json.data(), size)) break;
    auto command = Parse(json);
    if (!command) continue;
    const bool shutdown = command->type == "shutdown";
    Command* raw = command.release();
    if (!PostMessageW(receiver, kCommandMessage, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    if (shutdown) return;
  }
  PostMessageW(receiver, kShutdownMessage, 0, 0);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // The CRT-supplied raw string can differ between ShellExecute and
  // CreateProcess (including leading spaces or quotes). Parse argv once.
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return 1;
  const std::wstring arg = argc > 1 ? argv[1] : L"";
  LocalFree(argv);
  const std::wstring_view args = arg;
  if (args == L"--tray") return RunTray(instance);
  const bool testHost = args == L"--host-test";
  const bool nativeHost = testHost || args.rfind(L"chrome-extension://", 0) == 0;
  if (!nativeHost) return RunSetup(args == L"--setup-silent", args == L"--uninstall-silent");
  // "退出" from the tray also stops the actual pin service, without a
  // watchdog recreating it. It resumes at the next logon or manual tray start.
  if (!testHost && !IsTrayRunning()) return 1;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  g_input = GetStdHandle(STD_INPUT_HANDLE);
  g_output = GetStdHandle(STD_OUTPUT_HANDLE);
  // Chromium owns both pipe servers even when cmd.exe is the direct parent.
  ULONG inputPid = 0, outputPid = 0;
  if (testHost) g_browserPid = ParentProcessId();
  else if (GetNamedPipeServerProcessId(g_input, &inputPid) &&
           GetNamedPipeServerProcessId(g_output, &outputPid) && inputPid == outputPid)
    g_browserPid = inputPid;
  if (!g_browserPid) return 1;
  Gdiplus::GdiplusStartupInput startup;
  if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &startup, nullptr) != Gdiplus::Ok) return 1;
  WNDCLASSW dispatcher{};
  dispatcher.lpfnWndProc = DispatcherProc;
  dispatcher.hInstance = instance;
  dispatcher.lpszClassName = kDispatcherClass;
  WNDCLASSW button{};
  button.lpfnWndProc = ButtonProc;
  button.hInstance = instance;
  button.lpszClassName = kButtonClass;
  button.hCursor = LoadCursorW(nullptr, IDC_HAND);
  if (!RegisterClassW(&dispatcher) || !RegisterClassW(&button)) return 1;
  g_dispatcher = CreateWindowExW(0, kDispatcherClass, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, instance, nullptr);
  if (!g_dispatcher) return 1;
  for (const auto [first, last] : {
       std::pair<DWORD, DWORD>{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
       {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
       {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
       {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE}}) {
    HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr, OnWindowEvent, g_browserPid, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (hook) g_hooks.push_back(hook);
  }
  SetTimer(g_dispatcher, 1, 500, nullptr);
  Send("{\"type\":\"ready\"}");
  std::thread reader(ReadCommands, g_dispatcher);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  for (auto& [id, entry] : g_entries) Release(entry);
  for (auto hook : g_hooks) UnhookWinEvent(hook);
  CancelSynchronousIo(reader.native_handle());
  reader.join();
  // Dispose commands posted just before shutdown, before their HWND was destroyed.
  while (PeekMessageW(&message, nullptr, kCommandMessage, kCommandMessage, PM_REMOVE))
    delete reinterpret_cast<Command*>(message.lParam);
  Gdiplus::GdiplusShutdown(g_gdiplusToken);
  return 0;
}
