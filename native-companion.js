/* Optional Windows companion. PopTab itself never depends on this port. */
(() => {
  const HOST = "com.poptab.titlepin";
  const STORAGE_KEY = "cleanSessions";
  const RETRY_MS = 30000;
  const IDLE_MS = 10000;

  let port = null;
  let retryAfter = 0;
  let syncTimer = null;
  let idleTimer = null;
  let syncing = false;
  let syncAgain = false;
  let hadWindows = false;

  function cancelIdle() {
    if (idleTimer !== null) {
      clearTimeout(idleTimer);
      idleTimer = null;
    }
  }

  function scheduleSync(delay = 80) {
    if (syncTimer !== null) return;
    syncTimer = setTimeout(() => {
      syncTimer = null;
      void syncWindows();
    }, delay);
  }

  function connect() {
    if (port) return true;
    if (Date.now() < retryAfter) return false;

    try {
      const current = chrome.runtime.connectNative(HOST);
      port = current;

      current.onMessage.addListener((message) => {
        if (message?.type === "ready") scheduleSync(0);
      });

      current.onDisconnect.addListener(() => {
        if (port !== current) return;
        port = null;
        cancelIdle();
        /* A missing host is normal until the user installs the companion. */
        retryAfter = Date.now() + RETRY_MS;
        if (hadWindows) scheduleSync(RETRY_MS);
      });
      return true;
    } catch {
      retryAfter = Date.now() + RETRY_MS;
      return false;
    }
  }

  function send(message) {
    if (!port || !message) return false;
    try {
      port.postMessage(message);
      return true;
    } catch {
      return false;
    }
  }

  async function collectWindows() {
    const stored = await chrome.storage.local.get(STORAGE_KEY);
    const sessions = Object.values(stored[STORAGE_KEY] ?? {});
    const result = await Promise.all(sessions.map(async (session) => {
      const id = session?.popupWindowId;
      if (!Number.isInteger(id) || !Number.isInteger(session?.tabId)) {
        return null;
      }

      try {
        const [window, tab] = await Promise.all([
          chrome.windows.get(id),
          chrome.tabs.get(session.tabId)
        ]);
        if (window.type !== "popup" || tab.windowId !== id) return null;
        const numbers = [window.left, window.top, window.width, window.height];
        if (!numbers.every(Number.isFinite)) return null;
        return {
          id,
          left: Math.round(window.left),
          top: Math.round(window.top),
          width: Math.round(window.width),
          height: Math.round(window.height)
        };
      } catch {
        return null;
      }
    }));
    return result.filter(Boolean);
  }

  async function syncWindows() {
    if (syncing) {
      syncAgain = true;
      return;
    }
    syncing = true;
    try {
      const windows = await collectWindows();
      hadWindows = windows.length > 0;

      if (hadWindows) {
        cancelIdle();
        if (!port) connect();
      }

      if (port) send({ type: "sync", windows });

      if (!hadWindows && port && idleTimer === null) {
        /* Keep the host alive briefly across a next/previous-tab handoff. */
        idleTimer = setTimeout(() => {
          idleTimer = null;
          if (!hadWindows && port) {
            port.disconnect();
            port = null;
          }
        }, IDLE_MS);
      }
    } catch (error) {
      console.debug("PopTab native companion sync:", error);
    } finally {
      syncing = false;
      if (syncAgain) {
        syncAgain = false;
        scheduleSync(0);
      }
    }
  }

  chrome.storage.onChanged.addListener((changes, area) => {
    if (area === "local" && STORAGE_KEY in changes) scheduleSync();
  });
  chrome.windows.onBoundsChanged.addListener(() => scheduleSync());
  chrome.windows.onRemoved.addListener(() => scheduleSync());

  self.popTabNativeCompanion = {
    /* The old popup is destroyed on tab switch. Transfer its pin intentionally. */
    transfer(from, to) {
      if (!Number.isInteger(from) || !Number.isInteger(to) || from === to) return;
      send({ type: "transfer", from, to });
    }
  };

  scheduleSync(0);
})();
