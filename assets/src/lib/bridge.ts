export type HardwareState = "probing" | "available" | "unavailable";
export type BrightnessMode = "probing" | "hardware" | "software";

export interface MonitorData {
  uid: number;
  key: string;
  name: string;
  device: string;
  width: number;
  height: number;
  primary: boolean;
  hardware: HardwareState;
  mode: BrightnessMode;
  forceSoftware: boolean;
  // Desired brightness. 0..100 drives the backlight on hardware monitors;
  // negative values (only when allowed) add software dimming below the
  // backlight's minimum. Software-only monitors use min..100.
  value: number;
  min: number;
  max: number;
  error: string;
}

export interface ConfigData {
  allowBelowMinimum: boolean;
  startWithWindows: boolean;
  debugLog: boolean;
}

export interface InitData {
  config: ConfigData;
  monitors: MonitorData[];
  webView2Version: string;
}

type InitCallback = (data: InitData) => void;
type MonitorsCallback = (monitors: MonitorData[]) => void;

let initCallback: InitCallback | null = null;
let monitorsCallback: MonitorsCallback | null = null;

export function onInit(cb: InitCallback) {
  initCallback = cb;
}

export function onMonitors(cb: MonitorsCallback) {
  monitorsCallback = cb;
  return () => {
    if (monitorsCallback === cb) monitorsCallback = null;
  };
}

// Called by C via ExecuteScript
(window as unknown as Record<string, unknown>).onInit = (data: InitData) => {
  if (initCallback) initCallback(data);
};

(window as unknown as Record<string, unknown>).onMonitors = (
  monitors: MonitorData[]
) => {
  if (monitorsCallback) monitorsCallback(monitors);
};

function post(message: Record<string, unknown>) {
  window.chrome.webview.postMessage(JSON.stringify(message));
}

export function getInit() {
  post({ action: "getInit" });
}

export function setBrightness(uid: number, value: number) {
  post({ action: "setBrightness", uid, value });
}

export function setAllBrightness(value: number) {
  post({ action: "setAllBrightness", value });
}

export function setMonitorSoftwareOnly(uid: number, softwareOnly: boolean) {
  post({ action: "setMonitorSoftwareOnly", uid, softwareOnly });
}

export function setAllowBelowMinimum(enabled: boolean) {
  post({ action: "setAllowBelowMinimum", enabled });
}

export function refreshMonitors() {
  post({ action: "refreshMonitors" });
}

export function saveSettings(config: Pick<ConfigData, "startWithWindows" | "debugLog">) {
  post({
    action: "saveSettings",
    startWithWindows: config.startWithWindows,
    debugLog: config.debugLog,
  });
}

export function closeDialog() {
  post({ action: "close" });
}

// The height the page wants the host window to provide, in CSS pixels. The
// host converts it to physical pixels, adds the window frame, clamps the
// result to the monitor's work area, and re-centers the dialog.
export function reportSize(height: number) {
  post({ action: "resize", height });
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
