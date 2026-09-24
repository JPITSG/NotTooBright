// Run with node --test tests/test_ui.cjs after installing assets dependencies.
const assert = require('node:assert/strict');
const test = require('node:test');
const { loadTs } = require('./load_ts.cjs');

const { hasScheduleChanges, reconcileScheduleSelection } = loadTs('assets/src/lib/scheduleSelection.ts');
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

test('schedule edits require Save, including disabled settings; reverting clears them', () => {
  const saved = { enabled: false, latitude: '', longitude: '', dayLevel: 100, nightLevel: 30,
    dawnStartOffset: -30, dawnEndOffset: 30, duskStartOffset: -30, duskEndOffset: 30,
    cycleResetMinutes: 240, deepSleepEnabled: false, deepSleepLevel: 10,
    deepSleepMinutes: 1410, scheduledKeys: ['a', 'b'] };
  const monitors = [monitor('a', 1), monitor('b', 2)];
  for (const [key, value] of Object.entries(saved)) {
    const changed = { ...saved, [key]: typeof value === 'boolean' ? !value :
      typeof value === 'number' ? value + 1 : typeof value === 'string' ? '1' : ['b'] };
    assert.equal(hasScheduleChanges(changed, saved, monitors), true, key);
    changed[key] = value;
    assert.equal(hasScheduleChanges(changed, saved, monitors), false, key);
  }
});

test('monitor churn and persisted live changes do not create unsaved schedule edits', () => {
  const current = { enabled: true, scheduledKeys: ['a', 'b', 'gone', 'hidden'] };
  const saved = { enabled: true, scheduledKeys: ['b', 'a'] };
  const monitors = [monitor('b', 21), monitor('a', 22), monitor('hidden', 3, false, true)];
  assert.equal(hasScheduleChanges(current, saved, monitors), false);
  const live = monitors.map(m => ({ ...m, value: 40, forceSoftware: true, pausedUntil: '04:00' }));
  assert.equal(hasScheduleChanges(current, saved, live), false);
  assert.equal(hasScheduleChanges({ ...current, scheduledKeys: ['b'] }, saved, live), true);
  const newMonitor = monitor('new', 23);
  const selection = reconcileScheduleSelection(current.scheduledKeys, new Set(['a', 'b']), [...live, newMonitor]);
  assert.equal(hasScheduleChanges({ ...current, scheduledKeys: selection },
    { ...saved, scheduledKeys: [...saved.scheduledKeys, 'new'] }, [...live, newMonitor]), false);
});

test('native close requests reach the latest listener and stop after unmount', () => {
  const window = {};
  const { onCloseRequested } = loadTs('assets/src/lib/bridge.ts', { window });
  window.onCloseRequested();
  let first = 0, current = 0;
  const removeFirst = onCloseRequested(() => first++);
  window.onCloseRequested();
  const removeCurrent = onCloseRequested(() => current++);
  removeFirst();
  window.onCloseRequested();
  removeCurrent();
  window.onCloseRequested();
  assert.equal(first, 1);
  assert.equal(current, 1);
});

test('Save sends stable keys and limits changes to monitors shown in the dialog', () => {
  let message;
  const { saveSettings } = loadTs('assets/src/lib/bridge.ts', {
    window: { chrome: { webview: { postMessage: (text) => { message = JSON.parse(text); } } } },
  });
  saveSettings(false, false, true, true, false, '', [], { scheduledKeys: ['a', 'gone', 'hidden'] }, [
    monitor('a', 9), monitor('b', 10, false), monitor('hidden', 11, true, true),
  ]);
  assert.equal(message.scheduledKeys, 'a');
  assert.equal(message.scheduleMonitorKeys, 'a,b');
  assert.equal(message.startWithWindows, true);
  assert.equal(message.pauseInRemoteSession, true);
  assert.equal(message.brightnessKeys, false);
});

const solar = loadTs('assets/src/lib/solar.ts');
const defaultShape = { dayLevel: 100, nightLevel: 10, dawnStartOffset: -30,
  dawnEndOffset: 30, duskStartOffset: -30, duskEndOffset: 30,
  deepSleepEnabled: false, deepSleepLevel: 5, deepSleepMinutes: 1410 };

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

test('deep sleep fades in, lasts through midnight into the dawn ramp, and is drawn as a band', () => {
  const days = Array.from({ length: 5 }, () => ({ sunrise: 360, sunset: 1080, noon: 720, polar: 0 }));
  const shape = { ...defaultShape, nightLevel: 30, deepSleepEnabled: true, deepSleepLevel: 10 };
  assert.equal(solar.scheduleValueAt(shape, days, 1200), 30);
  assert.equal(solar.scheduleValueAt(shape, days, 1410), 30);
  assert.equal(solar.scheduleValueAt(shape, days, 1412.5), 20);
  assert.equal(solar.scheduleValueAt(shape, days, 1415), 10);
  assert.equal(solar.scheduleValueAt(shape, days, 1440), solar.scheduleValueAt(shape, days, 0));
  assert.equal(solar.scheduleValueAt(shape, days, 360), 55);
  assert.equal(solar.scheduleValueAt(shape, days, 390), 100);
  // The band runs from the start to where the morning ramp begins. Arrays from
  // the module's own context are copied for the comparison.
  const plain = (value) => JSON.parse(JSON.stringify(value));
  assert.deepEqual(plain(solar.deepSleepSpans(shape, days)), [[0, 330], [1410, 1440]]);
  assert.deepEqual(plain(solar.deepSleepSpans({ ...shape, deepSleepMinutes: 90 }, days)), [[90, 330]]);
  assert.deepEqual(plain(solar.deepSleepSpans({ ...shape, deepSleepMinutes: 360 }, days)), []);
  assert.deepEqual(plain(solar.deepSleepSpans({ ...shape, deepSleepEnabled: false }, days)), []);
  const polarNight = days.map((day) => ({ ...day, polar: -1, sunrise: 720, sunset: 720 }));
  assert.deepEqual(plain(solar.deepSleepSpans(shape, polarNight)), [[0, 720], [1410, 1440]]);
  assert.equal(solar.scheduleValueAt(shape, polarNight, 700), 10);
  assert.equal(solar.scheduleValueAt(shape, polarNight, 800), 30);
});

test('solar event times use local clock minutes across both DST changes', () => {
  // Reconstruct the event instant from the UTC result, independently of
  // the local implementation. Compare the local calendar and clock fields.
  for (const [zone, dates, lat, lon] of [
    ['Europe/Warsaw', [[2026, 2, 29], [2026, 9, 25]], 52.23, 21.01],
    ['Australia/Lord_Howe', [[2026, 3, 5], [2026, 9, 4]], -31.55, 159.08],
    ['Pacific/Apia', [[2026, 5, 21]], -13.83, -171.76],
  ]) {
    for (const [year, month, date] of dates) {
      process.env.TZ = 'UTC';
      const utc = solar.computeSolarDay(lat, lon, new Date(year, month, date, 12));
      process.env.TZ = zone;
      const local = solar.computeSolarDay(lat, lon, new Date(year, month, date, 12));
      for (const event of ['sunrise', 'sunset', 'noon']) {
        const instant = new Date(Date.UTC(year, month, date) + utc[event] * 60000);
        const delta = (Date.UTC(instant.getFullYear(), instant.getMonth(), instant.getDate()) -
          Date.UTC(year, month, date)) / 86400000;
        const expected = delta * 1440 + instant.getHours() * 60 + instant.getMinutes();
        assert.equal(local[event], expected, `${zone} ${month + 1}/${date} ${event}`);
      }
    }
  }
});
