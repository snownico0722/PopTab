const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const script = fs.readFileSync(path.join(__dirname, '../../native-companion.js'), 'utf8');
function event() {
  const listeners = [];
  return { addListener(f) { listeners.push(f); }, async fire(...args) { await Promise.all(listeners.map(f => f(...args))); } };
}
function setup() {
  const timers = new Map(), alarms = new Map(), messages = [], ports = [];
  let sequence = 0;
  const state = { missing: false, sessions: { 42: { tabId: 42, popupWindowId: 91 } }, focus: { id: 91, type: 'popup', focused: true } };
  const chrome = {
    runtime: { connectNative() {
      if (state.missing) throw Error('Host missing');
      const port = { onMessage: event(), onDisconnect: event(), postMessage: m => messages.push(m) };
      ports.push(port); return port;
    } },
    alarms: { onAlarm: event(), create: async (n, a) => alarms.set(n, a), clear: async n => alarms.delete(n) },
    storage: { onChanged: event(), local: { get: async () => ({ cleanSessions: state.sessions }) } },
    windows: { onFocusChanged: event(), onRemoved: event(), get: async id => ({ id, type: 'popup' }), getLastFocused: async () => state.focus },
    tabs: { get: async () => ({ windowId: 91 }) }
  };
  const self = {};
  vm.runInNewContext(script, { chrome, self, console, Date,
    setTimeout(f, delay) { const id = ++sequence; timers.set(id, { f, delay }); return id; },
    clearTimeout(id) { timers.delete(id); }
  });
  async function flush(maxDelay = 50) {
    for (let i = 0; i < 20; i++) {
      for (const [id, t] of [...timers]) if (t.delay <= maxDelay) { timers.delete(id); t.f(); }
      await new Promise(resolve => setImmediate(resolve));
    }
  }
  return { chrome, self, state, alarms, messages, ports, flush };
}
test('focus handshake confirms only a live, focused PopTab popup; old ports cannot answer', async () => {
  const e = setup(); await e.flush();
  assert.equal(e.messages[0].windows[0].id, 91);
  await e.ports[0].onMessage.fire({ type: 'focus', request: 8 });
  assert.equal(e.messages.at(-1).id, 91);
  e.state.focus = { id: 12, type: 'normal', focused: true };
  await e.ports[0].onMessage.fire({ type: 'focus', request: 9 });
  assert.equal(e.messages.at(-1).id, 0);
  e.state.focus = { id: 91, type: 'popup', focused: false };
  await e.ports[0].onMessage.fire({ type: 'focus', request: 10 });
  assert.equal(e.messages.at(-1).id, 0);
  await e.ports[0].onDisconnect.fire();
  const count = e.messages.length;
  await e.ports[0].onMessage.fire({ type: 'focus', request: 11 });
  assert.equal(e.messages.length, count);
});
test('missing host retries through a browser alarm and stops retrying with no popups', async () => {
  const e = setup(); e.state.missing = true; await e.flush();
  assert.ok(e.alarms.has('poptab-native-retry'));
  e.state.missing = false;
  await e.chrome.alarms.onAlarm.fire({ name: 'poptab-native-retry' }); await e.flush();
  assert.equal(e.ports.length, 1);
  await e.ports[0].onMessage.fire({ type: 'ready' }); await e.flush();
  assert.equal(e.alarms.size, 0);
  e.self.popTabNativeCompanion.transfer(91, 92);
  assert.equal(e.messages.at(-1).type, 'transfer');
  e.state.sessions = {};
  await e.chrome.storage.onChanged.fire({ cleanSessions: {} }, 'local'); await e.flush();
  await e.flush(10000);
  assert.equal(e.messages.at(-1).type, 'shutdown');
  await e.ports[0].onDisconnect.fire();
  assert.equal(e.alarms.size, 0);
});
