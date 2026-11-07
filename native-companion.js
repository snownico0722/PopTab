/* Optional companion. Missing host never blocks normal PopTab operations. */
(() => {
  const HOST = "com.poptab.titlepin";
  const RETRY = "poptab-native-retry";
  let port = null;
  let retryAfter = 0;
  let syncTimer = null;
  let idleTimer = null;
  let syncing = false;
  let syncAgain = false;
  let hadWindows = false;

  function scheduleSync() {
    if (syncTimer !== null) return;
    syncTimer = setTimeout(() => {
      syncTimer = null;
      void syncWindows();
    }, 50);
  }
  function retry() {
    if (!hadWindows) return;
    retryAfter = Date.now() + 60000;
    // A browser alarm survives service-worker suspension; a JS timer does not.
    void chrome.alarms.create(RETRY, { delayInMinutes: 1 });
  }
  function send(message) {
    if (!port) return;
    try { port.postMessage(message); } catch { /* onDisconnect owns recovery. */ }
  }
  async function collectWindows() {
    const stored = await chrome.storage.local.get("cleanSessions");
    const windows = await Promise.all(Object.values(stored.cleanSessions ?? {}).map(async (s) => {
      if (!Number.isInteger(s?.popupWindowId) || !Number.isInteger(s?.tabId)) return null;
      try {
        const [window, tab] = await Promise.all([
          chrome.windows.get(s.popupWindowId), chrome.tabs.get(s.tabId)
        ]);
        return window.type === "popup" && tab.windowId === window.id ? { id: window.id } : null;
      } catch { return null; }
    }));
    return windows.filter(Boolean);
  }
  function connect() {
    if (port || Date.now() < retryAfter) return;
    try {
      const current = chrome.runtime.connectNative(HOST);
      port = current;
      current.onMessage.addListener(async (message) => {
        if (port !== current) return;
        if (message?.type === "ready") {
          retryAfter = 0;
          void chrome.alarms.clear(RETRY);
          scheduleSync();
        } else if (message?.type === "focus" && Number.isInteger(message.request)) {
          try {
            const windows = await collectWindows();
            const focused = await chrome.windows.getLastFocused();
            if (port !== current) return;
            // Do not infer identity from identical rectangles or page titles.
            const id = focused.focused && focused.type === "popup" &&
              windows.some((w) => w.id === focused.id) ? focused.id : 0;
            send({ type: "focus", request: message.request, id });
          } catch { /* Window closed during the query; the next event will retry. */ }
        }
      });
      current.onDisconnect.addListener(() => {
        void chrome.runtime.lastError; // Missing host is expected before installation.
        if (port !== current) return;
        port = null;
        clearTimeout(idleTimer);
        idleTimer = null;
        retry();
      });
    } catch { retry(); }
  }
  async function syncWindows() {
    if (syncing) { syncAgain = true; return; }
    syncing = true;
    try {
      const windows = await collectWindows();
      hadWindows = windows.length > 0;
      if (hadWindows) {
        clearTimeout(idleTimer);
        idleTimer = null;
        connect();
      } else {
        void chrome.alarms.clear(RETRY);
      }
      send({ type: "sync", windows });
      if (!hadWindows && port && idleTimer === null) {
        // The connection keeps this worker alive during a next/previous-tab handoff.
        idleTimer = setTimeout(() => {
          idleTimer = null;
          if (!hadWindows) send({ type: "shutdown" });
        }, 10000);
      }
    } catch (error) {
      console.debug("PopTab companion:", error);
    } finally {
      syncing = false;
      if (syncAgain) { syncAgain = false; scheduleSync(); }
    }
  }
  chrome.alarms.onAlarm.addListener((alarm) => {
    if (alarm.name === RETRY) { retryAfter = 0; scheduleSync(); }
  });
  chrome.storage.onChanged.addListener((changes, area) => {
    if (area === "local" && "cleanSessions" in changes) scheduleSync();
  });
  chrome.windows.onFocusChanged.addListener(scheduleSync);
  chrome.windows.onRemoved.addListener(scheduleSync);
  self.popTabNativeCompanion = {
    transfer(from, to) {
      if (Number.isInteger(from) && Number.isInteger(to) && from !== to)
        send({ type: "transfer", from, to });
    }
  };
  scheduleSync();
})();
