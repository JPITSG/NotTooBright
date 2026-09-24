// Run after make, with Puppeteer Core available and CHROMIUM_PATH set.
// Uses the built UI and a mocked native WebView bridge; never touches Windows.
const assert = require('node:assert/strict');
const path = require('node:path');
const puppeteer = require('puppeteer-core');

(async () => {
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROMIUM_PATH, headless: true, args: ['--no-sandbox'],
  });
  try {
    const page = await browser.newPage();
    await page.setViewport({width: 1000, height: 900});
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    const config = {
      allowBelowMinimum: false, debugLog: false, startWithWindows: false,
      autoCheckForUpdates: false, pauseInRemoteSession: true, brightnessKeys: false,
      updateCheckPending: false, updatePromptPending: false,
      trayTarget: '*', trayPresets: '100,75,50',
      schedule: {enabled: true, hasLocation: true, latitude: 50, longitude: 20,
        dayLevel: 100, nightLevel: 30, dawnStartOffset: -30, dawnEndOffset: 30,
        duskStartOffset: -30, duskEndOffset: 30, cycleResetMinutes: 240,
        deepSleepEnabled: true, deepSleepLevel: 10, deepSleepMinutes: 1410},
    };
    const monitors = [1, 2].map(uid => ({uid, key: `monitor-${uid}`, name: `Monitor ${uid}`,
      device: `DISPLAY${uid}`, width: 1920, height: 1080, primary: uid === 1,
      hardware: 'available', builtin: false, mode: 'hardware', knownHardware: true,
      forceSoftware: false, hidden: false, scheduled: true, pausedUntil: '',
      value: 80, min: 0, max: 100, error: ''}));
    await page.evaluateOnNewDocument(() => {
      window.messages = [];
      window.chrome = {webview: {postMessage: text => window.messages.push(JSON.parse(text))}};
    });
    const reset = async () => {
      await page.goto('file://' + path.resolve(__dirname, '../assets/dist/index.html'));
      await page.waitForFunction(() => window.messages.some(m => m.action === 'getInit'));
      await page.evaluate((config, monitors) => window.onInit({config, monitors}), config, monitors);
      await page.waitForFunction(() => window.messages.some(m => m.action === 'configReady'));
    };
    const click = async (text, modal = false) => {
      const buttons = await page.$$(modal ? '[role="alertdialog"] button' : 'button');
      const button = (await Promise.all(buttons.map(async button => ({
        button, text: await button.evaluate(el => el.textContent),
      })))).find(button => button.text === text);
      assert.ok(button, text);
      await button.button.click();
    };
    const changeText = async (selector, value) => {
      await page.click(selector);
      await page.keyboard.down('Control');
      await page.keyboard.press('a');
      await page.keyboard.up('Control');
      await page.keyboard.press('Backspace');
      if (value) await page.type(selector, value);
      assert.equal(await page.$eval(selector, el => el.value), value, selector);
    };
    const changeTime = async (selector, value) => {
      await page.$eval(selector, (el, value) => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, value);
        el.dispatchEvent(new Event('input', {bubbles: true}));
      }, value);
    };
    const step = async (selector, key) => { await page.focus(selector); await page.keyboard.press(key); };
    const actions = () => page.evaluate(() => window.messages.filter(m =>
      ['close', 'saveSettings'].includes(m.action)));
    const prompt = async () => {
      await page.waitForSelector('#save-alert-title');
      assert.equal(await page.$eval('#save-alert-title', el => el.textContent), 'Unsaved changes');
      assert.equal(await page.$eval('#save-alert-message', el => el.textContent), 'Save changes before closing?');
      assert.deepEqual(await actions(), []);
      assert.equal(await page.$$eval('[role="alertdialog"]', el => el.length), 1);
      assert.equal(await page.$eval('#scheduleEnabled', el => !!el.closest('[inert]')), true);
    };
    const noPrompt = () => page.waitForFunction(() => !document.querySelector('#save-alert-title'));
    const nativeClose = () => page.evaluate(() => window.onCloseRequested());
    const closed = async (expectNoPrompt = true) => {
      await page.waitForFunction(() => window.messages.some(m => m.action === 'close'));
      assert.deepEqual(await actions(), [{action: 'close'}]);
      if (expectNoPrompt) await noPrompt();
    };

    for (const trigger of [() => click('Cancel'), nativeClose, () => page.keyboard.press('Escape')]) {
      await reset(); await trigger(); await closed();
    }

    // Each editable setting is dirty only until reverted.
    const settings = [
      ...['debugLog', 'autoCheckForUpdates', 'startWithWindows', 'pauseInRemoteSession',
        'brightnessKeys', 'scheduleEnabled', 'deepSleep', 'schedule-monitor-1'].map(id =>
        [() => page.click(`#${id}`), () => page.click(`#${id}`)]),
      [() => page.select('#trayTarget', ''), () => page.select('#trayTarget', '*')],
      [() => changeText('#trayPresets', '100, 50'), () => changeText('#trayPresets', '100, 75, 50')],
      [() => changeText('#latitude', '51'), () => changeText('#latitude', '50')],
      [() => changeText('#longitude', '21'), () => changeText('#longitude', '20')],
      ...['dayLevel', 'nightLevel', 'deepSleepLevel'].map(id =>
        [() => step(`#${id}`, 'ArrowLeft'), () => step(`#${id}`, 'ArrowRight')]),
      [() => changeTime('#deepSleepTime', '22:30'), () => changeTime('#deepSleepTime', '23:30')],
      [() => changeTime('#cycleReset', '05:00'), () => changeTime('#cycleReset', '04:00')],
    ];
    for (const [index, [edit, revert]] of settings.entries()) {
      await reset(); await edit(); await nativeClose(); await prompt();
      await nativeClose(); await prompt();
      await click('Keep editing', true); await noPrompt();
      assert.deepEqual(await actions(), []);
      await revert(); await click('Cancel');
      await closed().catch(error => { error.message = `Reverting setting ${index}: ${error.message}`; throw error; });
    }

    await reset();
    await page.click('#startWithWindows');
    await click('Cancel'); await prompt();
    assert.equal(await page.evaluate(() => document.activeElement.textContent), 'Keep editing');
    await page.keyboard.press('Tab'); await page.keyboard.press('Tab');
    assert.equal(await page.evaluate(() => document.activeElement.textContent), 'Save');
    await page.keyboard.press('Tab');
    assert.equal(await page.evaluate(() => document.activeElement.textContent), 'Keep editing');
    await page.keyboard.down('Shift'); await page.keyboard.press('Tab'); await page.keyboard.up('Shift');
    assert.equal(await page.evaluate(() => document.activeElement.textContent), 'Save');
    await page.mouse.click(8, 8); await prompt();
    await page.keyboard.press('Escape'); await noPrompt();
    assert.equal(await page.$eval('#startWithWindows', el => el.checked), true);
    await page.keyboard.press('Escape'); await prompt();
    await click('Discard', true); await closed(false);

    // Normal Save and the prompt's Save send exactly the same settings.
    let saved;
    for (const throughPrompt of [false, true]) {
      await reset(); await page.click('#startWithWindows');
      await changeText('#trayPresets', '100, 50');
      if (throughPrompt) { await nativeClose(); await prompt(); }
      await click('Save', throughPrompt);
      await page.waitForFunction(() => window.messages.some(m => m.action === 'saveSettings'));
      const messages = await actions();
      assert.equal(messages.length, 1);
      assert.equal(messages[0].action, 'saveSettings');
      assert.equal(messages[0].startWithWindows, true);
      assert.equal(messages[0].trayPresets, '100,50');
      if (saved) assert.deepEqual(messages, saved);
      saved = messages;
    }

    // Validation returns to the offending input and keeps the edits.
    for (const [selector, value] of [['#latitude', ''], ['#longitude', '181'], ['#trayPresets', '101']]) {
      await reset(); await changeText(selector, value); await nativeClose(); await prompt();
      await click('Save', true); await noPrompt();
      assert.deepEqual(await actions(), []);
      await page.waitForFunction(selector => document.activeElement === document.querySelector(selector), {}, selector);
      assert.equal(await page.$eval(selector, el => el.value), value);
    }

    // Live settings and monitor updates have already been saved by the host.
    for (const edit of [() => step('#brightness-1', 'ArrowLeft'), () => step('#brightness-all', 'ArrowLeft'),
      () => page.click('#allowBelowMinimum'), () => page.click('#software-1'), () => click('Rescan'),
      () => page.click('[aria-label="Hide Monitor 1"]')]) {
      await reset(); await edit(); await nativeClose(); await closed();
    }
    await reset();
    await page.evaluate(monitors => window.onMonitors(monitors), []);
    await page.evaluate(monitors => window.onMonitors(monitors), [...monitors].reverse().map(m =>
      ({...m, uid: m.uid + 10, value: 60, pausedUntil: '04:00'})));
    await page.waitForSelector('#brightness-11');
    await nativeClose(); await closed();
    await reset();
    await page.evaluate(monitors => window.onMonitors(monitors), [...monitors, {...monitors[0], uid: 3, key: 'new'}]);
    await page.waitForSelector('#schedule-monitor-3:checked');
    await nativeClose(); await closed();
    await reset(); await page.click('#schedule-monitor-1');
    await page.evaluate(monitors => window.onMonitors(monitors), []);
    await page.waitForSelector('#brightness-1', {hidden: true});
    await page.evaluate(monitors => window.onMonitors(monitors), monitors.map(m => ({...m, uid: m.uid + 10})));
    await page.waitForSelector('#schedule-monitor-11:not(:checked)');
    await nativeClose(); await prompt();

    // An IP lookup fills unsaved coordinates without saving the form.
    await reset();
    await page.evaluate(() => window.onLocationResult({status: 'ok', latitude: 52, longitude: 21,
      place: 'Example', source: 'test', cached: false, nextLookup: '12:00'}));
    await page.waitForFunction(() => document.getElementById('latitude').value === '52');
    await nativeClose(); await prompt();

    // Update notices wait behind the close question and use the identical overlay.
    await reset(); await page.click('#debugLog'); await nativeClose(); await prompt();
    const overlay = () => page.$eval('[role="alertdialog"]', el => getComputedStyle(el.parentElement).backgroundColor);
    const saveOverlay = await overlay();
    await page.evaluate(() => window.onUpdateResult({status: 'newer', title: 'Update available',
      message: 'A newer version is ready to install.', currentVersion: '0.0.45', remoteVersion: '0.0.46', automatic: true}));
    await prompt(); await click('Keep editing', true);
    await page.waitForSelector('#update-alert-title');
    assert.equal(await overlay(), saveOverlay);
    assert.equal(saveOverlay, 'rgba(0, 0, 0, 0.35)');
    await nativeClose(); await prompt(); await page.keyboard.press('Escape');
    await page.waitForSelector('#update-alert-title'); await click('Cancel', true);
    await page.waitForFunction(() => !document.querySelector('[role="alertdialog"]'));
    assert.equal(await page.$eval('#debugLog', el => el.checked), true);
    assert.deepEqual(await actions(), []);
    assert.deepEqual(errors, []);
    console.log('Unsaved settings, close routes, save/discard, keyboard, validation, live changes, monitor churn, and update overlap checks passed');
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
