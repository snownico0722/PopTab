"""Windows-only integration test: a fake Chromium-class popup + the native host."""
import ctypes
from ctypes import wintypes
import json
import struct
import subprocess
import sys
import time

u32 = ctypes.windll.user32
kernel = ctypes.windll.kernel32
HWND = wintypes.HWND
LPARAM = wintypes.LPARAM
WPARAM = wintypes.WPARAM
LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, HWND, wintypes.UINT, WPARAM, LPARAM)
ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, HWND, LPARAM)
WS_OVERLAPPEDWINDOW = 0x00CF0000
WS_VISIBLE = 0x10000000
WS_EX_TOPMOST = 0x00000008
GWL_EXSTYLE = -20
GW_OWNER = 4
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
SW_SHOW = 5
SW_MINIMIZE = 6
SW_RESTORE = 9

class WNDCLASS(ctypes.Structure):
    _fields_ = [
        ("style", wintypes.UINT),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wintypes.HINSTANCE),
        ("hIcon", wintypes.HICON),
        ("hCursor", wintypes.HANDLE),
        ("hbrBackground", wintypes.HBRUSH),
        ("lpszMenuName", wintypes.LPCWSTR),
        ("lpszClassName", wintypes.LPCWSTR),
    ]

u32.DefWindowProcW.argtypes = [HWND, wintypes.UINT, WPARAM, LPARAM]
u32.DefWindowProcW.restype = LRESULT
u32.CreateWindowExW.restype = HWND
u32.CreateWindowExW.argtypes = [
    wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, HWND,
    wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p,
]
u32.GetWindowLongPtrW.argtypes = [HWND, ctypes.c_int]
u32.GetWindowLongPtrW.restype = LRESULT
u32.GetWindowRect.argtypes = [HWND, ctypes.POINTER(wintypes.RECT)]
u32.GetWindowRect.restype = wintypes.BOOL
u32.GetWindow.argtypes = [HWND, wintypes.UINT]
u32.GetWindow.restype = HWND
u32.GetClassNameW.argtypes = [HWND, wintypes.LPWSTR, ctypes.c_int]
u32.GetClassNameW.restype = ctypes.c_int
u32.SendMessageW.argtypes = [HWND, wintypes.UINT, WPARAM, LPARAM]
u32.SendMessageW.restype = LRESULT
u32.IsWindowVisible.argtypes = [HWND]
u32.IsWindowVisible.restype = wintypes.BOOL
u32.EnumWindows.argtypes = [ENUMPROC, LPARAM]
u32.EnumWindows.restype = wintypes.BOOL
kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel.GetModuleHandleW.restype = wintypes.HMODULE

@WNDPROC
def window_proc(hwnd, message, wp, lp):
    return u32.DefWindowProcW(hwnd, message, wp, lp)

wc = WNDCLASS()
wc.lpfnWndProc = window_proc
wc.hInstance = kernel.GetModuleHandleW(None)
wc.lpszClassName = "Chrome_WidgetWin_1"
assert u32.RegisterClassW(ctypes.byref(wc)), "failed to register fake Chromium class"

target = u32.CreateWindowExW(
    0, wc.lpszClassName, "Simulated PopTab Window",
    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
    150, 120, 700, 450, None, None, wc.hInstance, None,
)
assert target, "failed to create fake popup"
u32.ShowWindow(target, SW_SHOW)
u32.SetForegroundWindow(target)
bounds = wintypes.RECT()
assert u32.GetWindowRect(target, ctypes.byref(bounds))

def get_button():
    matches = []
    @ENUMPROC
    def inspect(hwnd, unused):
        name = ctypes.create_unicode_buffer(80)
        u32.GetClassNameW(hwnd, name, 80)
        if name.value == "PopTabPinButton" and u32.GetWindow(hwnd, GW_OWNER) == target:
            matches.append(hwnd)
        return True
    u32.EnumWindows(inspect, 0)
    return matches[0] if matches else None

def wait_for(predicate, timeout=8):
    stop = time.monotonic() + timeout
    while time.monotonic() < stop:
        value = predicate()
        if value:
            return value
        time.sleep(0.08)
    raise AssertionError("timed out waiting for native titlebar pin")

def is_pinned():
    return bool(u32.GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST)

proc = subprocess.Popen(
    [sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
try:
    data = {"type": "sync", "windows": [{
        "id": 77, "left": bounds.left, "top": bounds.top,
        "width": bounds.right - bounds.left, "height": bounds.bottom - bounds.top
    }]}
    payload = json.dumps(data, separators=(",", ":")).encode()
    proc.stdin.write(struct.pack("<I", len(payload)) + payload)
    proc.stdin.flush()
    button = wait_for(get_button)
    assert not is_pinned(), "fake popup unexpectedly started topmost"
    coords = (10 | (10 << 16))
    u32.SendMessageW(button, WM_LBUTTONDOWN, 0, coords)
    u32.SendMessageW(button, WM_LBUTTONUP, 0, coords)
    wait_for(is_pinned)

    u32.SendMessageW(button, WM_LBUTTONDOWN, 0, coords)
    u32.SendMessageW(button, WM_LBUTTONUP, 0, coords)
    wait_for(lambda: not is_pinned())

    u32.ShowWindow(target, SW_MINIMIZE)
    wait_for(lambda: not u32.IsWindowVisible(button))
    u32.ShowWindow(target, SW_RESTORE)
    wait_for(lambda: u32.IsWindowVisible(button))
    print("Native Win32 pin integration passed")
finally:
    proc.stdin.close()
    proc.wait(timeout=10)
    u32.DestroyWindow(target)
