# Regression checks

Run `python3 -m unittest discover -s tests` with Python 3 and a host C compiler.
The native checks extract production C functions and run them with Windows
I/O stubbed, including the configuration close gate and update handoff.

After installing the assets dependencies, run `node --test tests/test_ui.cjs`
for schedule calculations, monitor selection, unsaved schedule detection,
and bridge messages.

After `make`, run `node tests/ui_config_close.cjs` with `puppeteer-core`
available to Node and `CHROMIUM_PATH` pointing to a Chromium executable.
It loads the built UI with a mocked WebView bridge and checks unsaved settings,
reverting edits, native and footer close requests, Save/Discard/Keep editing,
Escape and Tab, validation, monitor reconnects, live brightness controls,
and overlapping update notices.

These checks do not run the Windows application or verify real WebView2,
registry writes, display hardware, or executable replacement on Windows.
