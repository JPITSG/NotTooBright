export interface ConfigData {
  startWithWindows: boolean;
  debugLog: boolean;
}

export interface InitData {
  config: ConfigData;
  webView2Version: string;
}

type InitCallback = (data: InitData) => void;

let initCallback: InitCallback | null = null;

export function onInit(cb: InitCallback) {
  initCallback = cb;
}

// Called by C via ExecuteScript
(window as unknown as Record<string, unknown>).onInit = (data: InitData) => {
  if (initCallback) initCallback(data);
};

export function getInit() {
  window.chrome.webview.postMessage(JSON.stringify({ action: "getInit" }));
}

export function saveSettings(config: ConfigData) {
  window.chrome.webview.postMessage(
    JSON.stringify({
      action: "saveSettings",
      startWithWindows: config.startWithWindows,
      debugLog: config.debugLog,
    })
  );
}

export function closeDialog() {
  window.chrome.webview.postMessage(JSON.stringify({ action: "close" }));
}

// The height the page wants the host window to provide, in CSS pixels. The
// host converts it to physical pixels, adds the window frame, clamps the
// result to the monitor's work area, and re-centers the dialog.
export function reportSize(height: number) {
  window.chrome.webview.postMessage(
    JSON.stringify({ action: "resize", height })
  );
}

declare global {
  interface Window {
    chrome: {
      webview: {
        postMessage(message: string): void;
      };
    };
  }
}
