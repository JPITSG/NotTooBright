// Run with node --test tests/test_ui.cjs after installing assets dependencies.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const ts = require('../assets/node_modules/typescript');

function loadTs(relative, globals = {}) {
  const source = fs.readFileSync(path.join(__dirname, '..', relative), 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
  }).outputText;
  const exports = {};
  vm.runInNewContext(output, { exports, Date, Math, Set, ...globals });
  return exports;
}

const { reconcileScheduleSelection } = loadTs('assets/src/lib/scheduleSelection.ts');
const monitor = (key, uid, scheduled = true, hidden = false) => ({ key, uid, scheduled, hidden });

test('reconnecting with a new UID preserves both selected and deselected choices', () => {
  const seen = new Set(['a', 'b']);
  const disconnected = reconcileScheduleSelection(['a'], seen, []);
  const reconnected = reconcileScheduleSelection(disconnected, seen, [
    monitor('a', 8), monitor('b', 9),
  ]);
  assert.equal(JSON.stringify(reconnected), '["a"]');
});

test('newly shown monitors inherit saved selections; hidden monitors stay excluded', () => {
  const selection = reconcileScheduleSelection([], new Set(), [
    monitor('a', 1), monitor('b', 2, false), monitor('c', 3, true, true),
  ]);
  assert.equal(JSON.stringify(selection), '["a"]');
});

test('Save sends stable keys and limits changes to monitors shown in the dialog', () => {
  let message;
  const { saveSettings } = loadTs('assets/src/lib/bridge.ts', {
    window: { chrome: { webview: { postMessage: (text) => { message = JSON.parse(text); } } } },
  });
  saveSettings(false, false, '', [], { scheduledKeys: ['a', 'gone', 'hidden'] }, [
    monitor('a', 9), monitor('b', 10, false), monitor('hidden', 11, true, true),
  ]);
  assert.equal(message.scheduledKeys, 'a');
  assert.equal(message.scheduleMonitorKeys, 'a,b');
});
