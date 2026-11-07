"""Real Chromium -> extension -> Native Messaging -> HWND integration, not a fake browser."""
import ctypes
from ctypes import wintypes as W
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import winreg
from playwright.sync_api import sync_playwright

u = ctypes.windll.user32
H = W.HWND
L = ctypes.c_ssize_t
ENUM = ctypes.WINFUNCTYPE(W.BOOL, H, W.LPARAM)
u.EnumWindows.argtypes = [ENUM, W.LPARAM]
u.GetWindow.argtypes = [H, W.UINT]; u.GetWindow.restype = H
u.GetClassNameW.argtypes = [H, W.LPWSTR, ctypes.c_int]
u.GetWindowTextW.argtypes = [H, W.LPWSTR, ctypes.c_int]
u.GetWindowLongPtrW.argtypes = [H, ctypes.c_int]; u.GetWindowLongPtrW.restype = L
u.SetWindowPos.argtypes = [H, H, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, W.UINT]
u.SendMessageW.argtypes = [H, W.UINT, W.WPARAM, W.LPARAM]; u.SendMessageW.restype = L
u.ShowWindow.argtypes = [H, ctypes.c_int]
u.IsWindowVisible.argtypes = [H]; u.IsWindowVisible.restype = W.BOOL
u.SetForegroundWindow.argtypes = [H]
u.GetWindowRect.argtypes = [H, ctypes.POINTER(W.RECT)]

def windows():
    result = []
    @ENUM
    def visit(hwnd, _):
        name, title = ctypes.create_unicode_buffer(100), ctypes.create_unicode_buffer(512)
        u.GetClassNameW(hwnd, name, len(name)); u.GetWindowTextW(hwnd, title, len(title))
        result.append((hwnd, name.value, title.value))
        return True
    u.EnumWindows(visit, 0)
    return result

def target(title):
    return next((h for h, c, t in windows() if c == 'Chrome_WidgetWin_1' and title in t), None)

def buttons():
    result = {}
    for hwnd, cls, _ in windows():
        if cls == 'PopTabPinButton':
            result.setdefault(u.GetWindow(hwnd, 4), []).append(hwnd)
    return result

def pinned(hwnd):
    return bool(u.GetWindowLongPtrW(hwnd, -20) & 8)

def click(hwnd):
    point = 10 | (10 << 16)
    u.SendMessageW(hwnd, 0x0201, 0, point); u.SendMessageW(hwnd, 0x0202, 0, point)

def wait_for(predicate, message, timeout=15):
    stop = time.monotonic() + timeout
    while time.monotonic() < stop:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError(message + '\n' + repr(windows()))

exe = str(Path(sys.argv[1]).resolve())
key_path = r'Software\Google\Chrome\NativeMessagingHosts\com.poptab.titlepin'
try:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as key:
        saved = winreg.QueryValueEx(key, '')
except FileNotFoundError:
    saved = None
old_local = os.environ['LOCALAPPDATA']
context = None
with tempfile.TemporaryDirectory(prefix='PopTab-browser-') as temporary:
    os.environ['LOCALAPPDATA'] = temporary
    env = dict(os.environ)
    root = Path(temporary)
    extension = root / 'PopTabCompanion' / 'PopTab-extension'
    profile = root / 'Google' / 'Chrome' / 'User Data'
    try:
        assert subprocess.run([exe, '--setup-silent'], env=env, timeout=20).returncode == 7
        with sync_playwright() as p:
            def launch():
                return p.chromium.launch_persistent_context(str(profile), channel='chromium',
                    headless=False, env=env, args=[f'--disable-extensions-except={extension}',
                                                 f'--load-extension={extension}', '--no-first-run'])
            def worker_for(ctx):
                return ctx.service_workers[0] if ctx.service_workers else ctx.wait_for_event('serviceworker')
            # Let Chromium write its own real Preferences/Secure Preferences.
            context = launch()
            worker_for(context).evaluate('chrome.runtime.id')
            context.close(); context = None
            assert subprocess.run([exe, '--setup-silent'], env=env, timeout=20).returncode == 0
            context = launch()
            worker = worker_for(context)
            a = worker.evaluate('''async () => {
                const tab = await chrome.tabs.create({url:'data:text/html,<title>PopTab native test A</title>A'});
                await chrome.tabs.create({windowId:tab.windowId,url:'data:text/html,<title>PopTab native test NEXT</title>NEXT',active:false});
                const popup = await enterCleanMode(tab, await loadSessions());
                return {id:popup.id, tabId:tab.id};
            }''')
            a_hwnd = wait_for(lambda: target('PopTab native test A'), 'A HWND missing')
            a_button = wait_for(lambda: buttons().get(a_hwnd), 'A button missing')[0]
            click(a_button); wait_for(lambda: pinned(a_hwnd), 'A did not pin')
            # B starts already topmost, before PopTab takes control of it.
            b = worker.evaluate('''async () => {
                const w = await chrome.windows.create({type:'popup',url:'data:text/html,<title>PopTab native test B</title>B',focused:true});
                return {id:w.id, tabId:w.tabs[0].id};
            }''')
            b_hwnd = wait_for(lambda: target('PopTab native test B'), 'B HWND missing')
            u.SetWindowPos(b_hwnd, H(-1), 0, 0, 0, 0, 0x0013)
            worker.evaluate('''async b => {
                const sessions = await loadSessions();
                sessions[String(b.tabId)] = {tabId:b.tabId,popupWindowId:b.id};
                await saveSessions(sessions);
            }''', b)
            b_button = wait_for(lambda: buttons().get(b_hwnd), 'B button missing')[0]
            worker.evaluate('''async ids => {
                for (const id of ids) await chrome.windows.update(id,{left:150,top:120,width:700,height:450});
            }''', [a['id'], b['id']])
            assert len(buttons().get(a_hwnd, [])) == len(buttons().get(b_hwnd, [])) == 1
            click(b_button); wait_for(lambda: not pinned(b_hwnd), 'B did not unpin independently')
            assert pinned(a_hwnd)
            # A normal browser window with the same bounds must not acquire a button.
            decoy = worker.evaluate('''async () => (await chrome.windows.create({type:'normal',
                url:'data:text/html,<title>PopTab native test NORMAL</title>normal',
                left:150,top:120,width:700,height:450,focused:true})).id''')
            normal = wait_for(lambda: target('PopTab native test NORMAL'), 'Normal HWND missing')
            time.sleep(0.5)
            assert normal not in buttons(), 'Normal browser was bound as a PopTab popup'
            # Exercise the real existing tab-switch implementation, not just transfer messages.
            worker.evaluate('''async a => {
                await chrome.windows.update(a.id,{focused:true});
                await switchCleanTab(1, await chrome.tabs.get(a.tabId));
            }''', a)
            next_hwnd = wait_for(lambda: target('PopTab native test NEXT'), 'Next popup missing')
            wait_for(lambda: buttons().get(next_hwnd), 'Handoff button missing')
            assert a_hwnd not in buttons(), 'Old popup button survived handoff'
            wait_for(lambda: pinned(next_hwnd), 'Handoff lost topmost state')
            next_button = buttons()[next_hwnd][0]
            u.ShowWindow(next_hwnd, 6)
            wait_for(lambda: not u.IsWindowVisible(next_button), 'Minimize left a visible button')
            u.ShowWindow(next_hwnd, 9)
            wait_for(lambda: u.IsWindowVisible(next_button), 'Restore lost the button')
            # Uninstall while browser windows stay alive: restore only our changes.
            assert subprocess.run([exe, '--uninstall-silent'], env=env, timeout=20).returncode == 0
            wait_for(lambda: not buttons(), 'Buttons survived uninstall')
            assert not pinned(next_hwnd), 'Our topmost change survived normal shutdown'
            assert pinned(b_hwnd), 'Original external topmost state was not restored'
            assert normal is not None and context.pages
            print('REAL Chromium native messaging, overlapping popups, normal-window exclusion, '
                  'handoff, minimize/restore and uninstall cleanup passed.')
            context.close(); context = None
    finally:
        if context is not None:
            try:
                context.close()
            except Exception:
                pass  # Playwright may already be stopped; preserve the original test failure.
        os.environ['LOCALAPPDATA'] = old_local
        try: winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key_path)
        except FileNotFoundError: pass
        if saved is not None:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key_path) as key:
                winreg.SetValueEx(key, '', 0, saved[1], saved[0])
