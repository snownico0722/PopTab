// Node's built-in test runner exercises the optional bridge without a browser.
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");

const script = fs.readFileSync(
  path.join(__dirname, "..", "..", "native-companion.js"), "utf8");

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function event() {
  const listeners = [];
  return {
    addListener(callback) { listeners.push(callback); },
    fire(...args) { for (const callback of listeners) callback(...args); }
  };
}

function setup({ missingHost = false } = {}) {
  const messages = [];
  const port = {
    onMessage: event(),
    onDisconnect: event(),
    postMessage(message) { messages.push(message); },
    disconnect() { port.onDisconnect.fire(); }
  };
  const session = {
    tabId: 42, popupWindowId: 91, sourceWindowId: 14
  };
  const storageChanged = event();
  const mockChrome = {
    runtime: {
      connectNative(host) {
        assert.equal(host, "com.poptab.titlepin");
        if (missingHost) throw new Error("host not found");
        return port;
      }
    },
    storage: {
      onChanged: storageChanged,
      local: { async get() { return { cleanSessions: { "42": session } }; } }
    },
    windows: {
      onBoundsChanged: event(),
      onRemoved: event(),
      async get() {
        return { type: "popup", left: 100, top: 200, width: 900, height: 600 };
      }
    },
    tabs: {
      async get() { return { windowId: 91 }; }
    }
  };
  const self = {};
  vm.runInNewContext(script, {
    chrome: mockChrome, self, console, Date, setTimeout, clearTimeout
  });
  return { self, messages, mockChrome, port };
}

test("sync reports only tracked PopTab popup geometry", async () => {
  const { messages } = setup();
  await delay(100);
  const sync = messages.find((message) => message.type === "sync");
  assert.ok(sync, "expected native sync");
  assert.deepEqual(JSON.parse(JSON.stringify(sync.windows)), [
    { id: 91, left: 100, top: 200, width: 900, height: 600 }
  ]);
});

test("explicit tab handoff is forwarded to the native host", async () => {
  const { self, messages } = setup();
  await delay(100);
  self.popTabNativeCompanion.transfer(91, 92);
  assert.ok(messages.some(
    (m) => m.type === "transfer" && m.from === 91 && m.to === 92
  ));
  self.popTabNativeCompanion.transfer(91, 91);
  assert.equal(messages.filter((m) => m.type === "transfer").length, 1);
});

test("missing native host leaves regular PopTab behavior alone", async () => {
  const { messages } = setup({ missingHost: true });
  await delay(100);
  assert.equal(messages.length, 0);
});
