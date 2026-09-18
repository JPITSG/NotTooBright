# NotTooBright

A lightweight Windows system tray application for lowering the brightness of
desktop monitors from software, for displays that have no native Windows
brightness control.

This is the initial scaffold: the tray application, its configuration
dialog, and the build. Brightness control itself is being added on top of it.

## Features

- **System Tray Integration** - Runs in the system tray with no main window; double-click the icon to open the configuration dialog
- **Configurable** - Settings are edited in a WebView2-based configuration dialog
- **Start at Sign-in** - Optionally registers itself in the current user's startup programs
- **Registry Storage** - Settings persist in the Windows Registry (`HKCU\SOFTWARE\JPIT\NotTooBright`)
- **Single Instance** - Only one instance can run at a time
- **Single File** - The executable embeds its icon, manifest, WebView2 loader, and configuration UI; nothing else needs to be installed alongside it
- **Debug Log** - Optional diagnostic log for reporting issues

## Context Menu Options

Right-click the tray icon to access:

- **Configure** - Opens the settings dialog (also opened by double-clicking the tray icon)
- **Exit** - Closes the application

## Configuration

Settings available in the Configure dialog:

| Setting | Description |
|---------|-------------|
| Start NotTooBright when you sign in | Adds the application to the current user's startup programs (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`) so it is in the system tray after every sign-in. The entry is refreshed on every start so it keeps pointing at the executable's current location. Disabled by default. |
| Enable debug logging | Appends timestamped diagnostic events to `%LOCALAPPDATA%\NotTooBright\debug.log` (rotated at ~1 MB). Useful when reporting issues. Disabled by default. |

The footer displays the application and WebView2 runtime versions together as
`v<application version> / <WebView2 version>`.

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
| `NotTooBright.c` | Application source (tray icon, configuration dialog host, settings) |
| `NotTooBright.manifest` | DPI awareness and supported OS manifest, embedded as a resource |
| `resource.rc`, `resource.h` | Resource script: icon, manifest, version info, embedded UI and loader |
| `version.h` | Single source of truth for the application version |
| `assets/` | Configuration UI (React, Vite, Tailwind) plus the icon and `WebView2Loader.dll` |
| `releases/` | Build output (`NotTooBright.exe`) |

## License

[MIT](LICENSE)
