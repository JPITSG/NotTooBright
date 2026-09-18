# Not Too Bright

A lightweight Windows system tray application for lowering the brightness of
desktop monitors from software, for displays that have no native Windows
brightness control.

## Features

- **Hardware Brightness (DDC/CI)** - Drives the monitor's own backlight through the Windows Monitor Configuration API, exactly like the buttons on the monitor, for every monitor that answers DDC/CI
- **Software Dimming Fallback** - Monitors that do not support DDC/CI (or that you switch to software mode) are dimmed with a click-through overlay that works on any display, GPU, dock, or remote session
- **Below the Hardware Minimum** - Optionally continue below 0% on hardware-controlled monitors: the backlight stays at its minimum and software dimming is added on top
- **One Slider per Monitor** - Plus an "All monitors" slider; changes apply immediately while dragging
- **Hide Monitors** - Restore a monitor's original brightness, then remove it from the list and stop controlling it entirely until the next rescan; the last monitor can never be hidden
- **Automatic Brightness (optional)** - Follows the sun for your latitude and longitude: a daytime level after sunrise, a night level after sunset, smooth transitions at dawn and dusk, shown on an editable graph of today's curve; a manual change pauses it until a reset time of your choosing
- **Keyboard Brightness Keys (optional)** - The Brightness Up / Brightness Down keys found on many keyboards step every listed monitor, whatever window is focused
- **Remembered per Monitor** - Each monitor is identified by its EDID (model and serial), so settings follow the monitor and are re-applied after sleep, after the display turns back on, and after display changes
- **Never Black** - Software dimming stops at 10% apparent brightness, and overlays vanish with the process, so a screen can never be left dark
- **Screenshot Friendly** - Overlays are excluded from screen capture and screen sharing (Windows 10 2004+), so screenshots show the undimmed picture
- **System Tray Integration** - Runs in the system tray with no main window; click the icon to open the brightness dialog
- **Registry Storage** - Settings persist in the Windows Registry (`HKCU\SOFTWARE\JPIT\NotTooBright`)
- **Single Instance** - Only one instance can run at a time
- **Single File** - The executable embeds its icon, manifest, WebView2 loader, and configuration UI; nothing else needs to be installed alongside it
- **Debug Log** - Optional diagnostic log of monitor detection and DDC/CI results for reporting issues

## Context Menu Options

Right-click the tray icon to access:

- **Configure** - Opens the brightness and settings dialog (also opened by clicking the tray icon)
- **Exit** - Closes the application; software dimming is removed, hardware brightness stays as set

## How Brightness Control Works

For every connected monitor Not Too Bright first tries **DDC/CI**, the control
channel monitors expose over their video cable. It reads and writes the
standard VCP brightness code (0x10), so the change is the same one you would
make with the monitor's own buttons: the backlight actually gets dimmer, with
no loss of contrast. All DDC/CI traffic runs on a background thread with
retries, because a single command can take a while and some monitors are slow
or flaky.

When a monitor does not answer (DDC/CI disabled in its on-screen menu, some
docks, KVMs, USB-C hubs, DisplayLink adapters, virtual machines, remote
sessions), the monitor is switched to **software dimming**: a topmost,
click-through black overlay whose transparency follows the slider. It cannot
lower the backlight, but it works everywhere, needs no driver support, and
never affects screenshots or screen sharing. Monitors that repeatedly fail
DDC/CI writes fall back to software automatically until the next rescan.

Both methods are behind the same per-monitor slider. With **Allow dimming
below the hardware minimum** enabled, the slider of a hardware-controlled
monitor extends to -90%: values below 0% keep the backlight at its minimum and
add software dimming on top, for monitors whose lowest setting is still too
bright.

Each monitor is identified by the model and serial number in its EDID, so the
chosen brightness is remembered per monitor and re-applied whenever monitors
are re-detected: after resuming from sleep, after the displays are switched
back on, after resolution or layout changes, and on every start. The first
time a monitor is seen its current hardware brightness is adopted as is, so
nothing changes until you move the slider, and that original value is
recorded for good: hiding the monitor later restores it before the
application lets go of the monitor.

## Automatic Brightness

Enable **Adjust brightness automatically with the sun** in the dialog, enter
your latitude and longitude (decimal degrees, north and east positive; a
nearby city is close enough), and choose a daytime and a night level. From
then on the selected monitors are brightened to the daytime level after
sunrise and dimmed to the night level after sunset, every day of the year,
using sunrise and sunset times computed for your location and the current
date.

The graph shows today's curve: the shaded band is daylight, the dashed
lines mark sunrise and sunset, and the blue marker is the current moment.
The four points shape the transitions - when the morning ramp starts and
reaches full brightness, and when the evening ramp starts and reaches the
night level. Drag them to taste; they are stored as offsets from sunrise and
sunset, so the curve keeps following the seasons instead of freezing at a
clock time. **Reset curve** restores the defaults (half an hour on either
side of sunrise and sunset). Where the sun does not rise or set on a given
day, the night or daytime level simply applies all day.

The schedule is evaluated every 30 seconds and writes to a monitor only when
the rounded percentage changes. If you change a scheduled monitor's
brightness by hand (its slider or **All monitors**), automatic control of
that monitor pauses until the **cycle reset time** you set (04:00 by
default), so an adjustment you make in the evening is not undone a minute
later; the card shows *Auto paused until HH:MM* with a **Resume now** link,
and saving the schedule again also resumes every monitor. Hidden monitors are
never scheduled. The night level can only go below 0% when **Allow dimming
below the hardware minimum** is enabled, and each monitor clamps the
scheduled value to its own range.

## Configuration

Click the tray icon (or choose **Configure**) to open the dialog. The
brightness section applies immediately; the settings below it are saved with
**Save**.

| Control | Description |
|---------|-------------|
| All monitors | Sets every monitor to the same value (shown only with more than one monitor). |
| Per-monitor slider | The brightness of that monitor. The badge shows how it is controlled: **Hardware (DDC/CI)**, **Software (no DDC/CI)**, or **Software (chosen)**. |
| Software dimming only | Shown for hardware-capable monitors. Uses the overlay instead of DDC/CI and leaves the monitor's own brightness setting untouched. Useful for monitors that answer DDC/CI but ignore or mangle the values. |
| Hide | Puts the monitor back to the brightness it had when Not Too Bright first saw it, then removes it from the list and stops controlling it entirely, as if it were not connected: its dimming overlay is removed, it is no longer probed, and no further brightness changes are sent. The last monitor in the list cannot be hidden. Hidden monitors stay hidden across restarts and display changes until you choose **Rescan**. |
| Rescan | Re-detects monitors, probes DDC/CI again (for example after enabling DDC/CI in a monitor's menu), and shows every hidden monitor again. |
| Allow dimming below the hardware minimum | Extends hardware-controlled sliders below 0% into software dimming (down to -90%). Applies immediately. Disabled by default. |
| Adjust brightness automatically with the sun | Enables the sun-based schedule described in [Automatic Brightness](#automatic-brightness). Requires a latitude and longitude. Saved with **Save**. Disabled by default. |
| Latitude / Longitude | Your location in decimal degrees (north and east positive), used to compute sunrise and sunset. |
| Daytime / Night brightness | The levels the schedule applies during the day and at night. |
| Today's curve | Graph of the resulting brightness over today; drag the four points to move the dawn and dusk transitions. |
| Apply to | Which monitors follow the schedule. Newly enabled schedules select every visible monitor. |
| Cycle reset time | Time of day at which monitors that were adjusted by hand return to automatic control. 04:00 by default. |
| Use the keyboard's brightness keys | Reacts to the Brightness Up / Brightness Down keys (HID consumer-control keys found on many desktop and laptop keyboards) wherever they are pressed: every listed monitor moves by the chosen step (1–25%, 5% by default) from its own current value, and holding a key repeats. Counts as a manual change for scheduled monitors. Windows continues to handle a laptop's built-in display on its own. Saved with **Save**. Disabled by default. |
| Enable debug logging | Appends timestamped diagnostic events (monitor detection, DDC/CI probe and write results, fallbacks, power events) to `%LOCALAPPDATA%\NotTooBright\debug.log` (rotated at ~1 MB). Useful when reporting issues. Disabled by default. |

The footer displays the application version.

Per-monitor values are stored under `HKCU\SOFTWARE\JPIT\NotTooBright\Monitors\<monitor id>`;
the schedule itself is stored under `HKCU\SOFTWARE\JPIT\NotTooBright`.

### Limitations

- Software dimming blends the picture toward black; it cannot reduce the
  backlight, so contrast drops as you dim, and the mouse cursor stays bright.
- Windows keeps some of its own surfaces (Start menu, notification center,
  secure desktop prompts) above every application window; those are not
  dimmed by the overlay. Hardware control has no such limit.
- DDC/CI must be enabled in the monitor's menu and pass through whatever
  sits between the computer and the monitor; many docks and KVM switches do
  not forward it.
- Brightness keys are only seen when the keyboard reports them as standard
  consumer-control keys. Laptop Fn combinations that go straight to the
  firmware, and keys that vendor software remaps, never reach applications.

## Icon Customization

The application uses a single icon file (`assets/icon.ico`) for the system
tray and the configuration dialog's title bar.

### Replacing the Icon

1. Replace `assets/icon.svg` with your own SVG file
2. Run `make icon` to generate `assets/icon.ico` (requires ImageMagick)
3. Rebuild with `make`

This generates a multi-resolution `.ico` containing 16x16, 24x24, 32x32,
48x48, and 256x256 sizes, covering all DPI scaling levels. The icon is
embedded into the executable at compile time via `resource.rc`.

## Requirements

- Windows 10/11 (64-bit)
- [WebView2 Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/) (usually pre-installed on Windows 10/11); only needed for the configuration dialog
- For hardware control: a monitor with DDC/CI enabled, connected so that DDC/CI reaches it (see [Limitations](#limitations)); everything else falls back to software dimming

## Building from Source

The application is cross-compiled from Linux. The build host needs:

- MinGW-w64 (`x86_64-w64-mingw32-gcc` and `x86_64-w64-mingw32-windres`)
- Node.js and npm (for the configuration UI in `assets/`)
- ImageMagick (optional, only for `make icon`)

```bash
# Build everything: configuration UI, resources, executable
make

# Output: releases/NotTooBright.exe (single self-contained file)
```

The MinGW toolchain is taken from the `PATH` by default. To use a toolchain
installed elsewhere, either pass a prefix on the command line:

```bash
make CROSS=/opt/mingw-w64/bin/x86_64-w64-mingw32-
```

or create an untracked `local.mk` next to the `Makefile` with the same
setting, so plain `make` picks it up on that machine:

```make
CROSS = /opt/mingw-w64/bin/x86_64-w64-mingw32-
```

Other targets:

```bash
make frontend   # build only the configuration UI (assets/dist/index.html)
make icon       # regenerate assets/icon.ico from assets/icon.svg
make clean      # remove objects and the built UI
make clean-all  # additionally remove node_modules and releases/
```

The WebView2 COM interfaces used by the configuration dialog are declared
directly in `NotTooBright.c`, so no WebView2 SDK download is required.

## Project Structure

| Path | Description |
|------|-------------|
| `NotTooBright.c` | Application source (monitor detection, DDC/CI worker, dimming overlays, tray icon, configuration dialog host, settings) |
| `NotTooBright.manifest` | DPI awareness and supported OS manifest, embedded as a resource |
| `resource.rc`, `resource.h` | Resource script: icon, manifest, version info, embedded UI and loader |
| `version.h` | Single source of truth for the application version |
| `assets/` | Configuration UI (React, Vite, Tailwind) plus the icon and `WebView2Loader.dll` |
| `releases/` | Build output (`NotTooBright.exe`) |

## License

[MIT](LICENSE)
