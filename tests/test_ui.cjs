// Run with node --test tests/test_ui.cjs after installing assets dependencies.
const assert = require('node:assert/strict');
const test = require('node:test');
const { loadTs } = require('./load_ts.cjs');

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

const solar = loadTs('assets/src/lib/solar.ts');
const defaultShape = { dayLevel: 100, nightLevel: 10, dawnStartOffset: -30,
  dawnEndOffset: 30, duskStartOffset: -30, duskEndOffset: 30 };

test('overlapping dusk and dawn remain continuous across calendar midnight', () => {
  process.env.TZ = 'Atlantic/Reykjavik';
  const shape = { ...defaultShape, dawnStartOffset: -360, duskEndOffset: 360 };
  const previous = solar.computeSolarDays(64.15, -21.94, new Date(2026, 5, 21, 12));
  const next = solar.computeSolarDays(64.15, -21.94, new Date(2026, 5, 22, 12));
  assert.equal(solar.scheduleValueAt(shape, previous, 1440), solar.scheduleValueAt(shape, next, 0));
  assert.ok(Math.abs(solar.scheduleValueAt(shape, previous, 1439.5) - solar.scheduleValueAt(shape, next, 0)) <= 1);
  // The overlap rule uses daytime contribution, not the brighter numeric value.
  const inverse = { ...shape, dayLevel: 10, nightLevel: 100 };
  assert.equal(solar.scheduleValueAt(inverse, previous, 1440), solar.scheduleValueAt(inverse, next, 0));
});

test('ordinary dawn, daytime, dusk and night levels stay unchanged', () => {
  const days = Array.from({ length: 5 }, () => ({ sunrise: 360, sunset: 1080, noon: 720, polar: 0 }));
  assert.equal(solar.scheduleValueAt(defaultShape, days, 0), 10);
  assert.equal(solar.scheduleValueAt(defaultShape, days, 360), 55);
  assert.equal(solar.scheduleValueAt(defaultShape, days, 720), 100);
  assert.equal(solar.scheduleValueAt(defaultShape, days, 1080), 55);
  assert.equal(solar.scheduleValueAt(defaultShape, days, 1440), 10);
});
