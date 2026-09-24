export type HardwareState = "probing" | "available" | "unavailable";
// "waiting": a monitor that has answered DDC/CI (or Windows' brightness
// control) before is not responding; it is left alone (no software dimming
// on top of its backlight) and retried automatically.
export type BrightnessMode = "probing" | "hardware" | "software" | "waiting";

export interface MonitorData {
  uid: number;
  key: string;
  name: string;
  device: string;
  width: number;
  height: number;
  primary: boolean;
  hardware: HardwareState;
  // A built-in display (a laptop's own panel): its hardware control is
  // Windows' own brightness control rather than DDC/CI.
  builtin: boolean;
  mode: BrightnessMode;
  // Has answered DDC/CI or Windows' brightness control at some point
  // (remembered across restarts).
  knownHardware: boolean;
  forceSoftware: boolean;
  // Removed from the dialog and left alone until the next rescan.
  hidden: boolean;
  // Follows the sun-based schedule; pausedUntil is "HH:MM" while a manual
  // change has paused the schedule (for every scheduled monitor), otherwise "".
  scheduled: boolean;
  pausedUntil: string;
  // Desired brightness. 0..100 drives the backlight on hardware monitors;
  // negative values (only when allowed) add software dimming below the
  // backlight's minimum. Software-only monitors use min..100.
  value: number;
  min: number;
  max: number;
  error: string;
}

export interface ScheduleData {
  enabled: boolean;
  hasLocation: boolean;
  latitude: number;
  longitude: number;
  dayLevel: number;
  nightLevel: number;
  dawnStartOffset: number;
  dawnEndOffset: number;
  duskStartOffset: number;
  duskEndOffset: number;
  cycleResetMinutes: number;
  // Optional: from deepSleepMinutes (after midnight) the level fades to
  // deepSleepLevel and stays there until the next morning's transition.
  deepSleepEnabled: boolean;
  deepSleepLevel: number;
  deepSleepMinutes: number;
}

export interface ConfigData {
  allowBelowMinimum: boolean;
  debugLog: boolean;
  autoCheckForUpdates: boolean;
  // Whether this user's Run entry launches this copy of the executable at
  // sign-in; read from the entry itself each time the dialog opens.
  startWithWindows: boolean;
  // Leave the monitors alone while the session is viewed through Remote
  // Desktop (the default); remoteSession says whether that is the case now.
  pauseInRemoteSession: boolean;
  remoteSession: boolean;
  // A "Detect from IP" lookup is still running (the dialog was reopened).
  locationDetecting: boolean;
  // The keyboard's Brightness Up/Down keys step every monitor by 10%
  // (built-in displays are left to Windows, which moves them itself).
  brightnessKeys: boolean;
  updateCheckPending: boolean;
  updatePromptPending: boolean;
  // Tray menu Increase/Decrease target: "" (items hidden), "*" (all
  // visible monitors), or a monitor key.
  trayTarget: string;
  // Preset levels listed between Increase and Decrease, "100,75,50".
  trayPresets: string;
  schedule: ScheduleData;
}

export const TRAY_TARGET_NONE = "";
export const TRAY_TARGET_ALL = "*";
export const TRAY_MAX_PRESETS = 20;

/** Parses the preset field: whole numbers from `min` to 100 separated by
 * commas (blank entries ignored, duplicates dropped, order kept). `error`
 * is set when anything else is in the text; `values` holds what did parse. */
export function parseTrayPresets(
  text: string,
  min: number
): { values: number[]; error: string | null } {
  const values: number[] = [];
  let error: string | null = null;
  for (const raw of text.split(",")) {
    const token = raw.trim();
    if (!token) continue;
    if (!/^-?\d+$/.test(token)) {
      error = "Use whole numbers separated by commas.";
      continue;
    }
    const value = Number(token);
    if (value < min || value > 100) {
      error = `Preset levels must be between ${min} and 100.`;
      continue;
    }
    if (!values.includes(value)) values.push(value);
  }
  if (!error && values.length > TRAY_MAX_PRESETS) {
    error = `Enter at most ${TRAY_MAX_PRESETS} preset levels.`;
    values.length = TRAY_MAX_PRESETS;
  }
  return { values, error };
}

export interface ScheduleSettings {
  enabled: boolean;
  latitude: string;
  longitude: string;
  dayLevel: number;
  nightLevel: number;
  dawnStartOffset: number;
  dawnEndOffset: number;
  duskStartOffset: number;
  duskEndOffset: number;
  cycleResetMinutes: number;
  deepSleepEnabled: boolean;
  deepSleepLevel: number;
  deepSleepMinutes: number;
  scheduledKeys: string[];
}

export interface InitData {
  config: ConfigData;
  monitors: MonitorData[];
  updateCompletedVersion: string;
}

export interface UpdateResult {
  status:
    | "newer"
    | "same"
    | "older"
    | "cancelled"
    | "error"
    | "completed";
  title: string;
  message: string;
  currentVersion: string;
  remoteVersion: string;
  automatic: boolean;
}

export interface UpdateProgress {
  kilobytesPerSecond: number;
}

// Answer to "Detect from IP". The host asks four free services at once and
// takes the first answer; a found location is reused for an hour (cached).
export interface LocationResult {
  status: "ok" | "failed" | "timeout" | "cancelled";
  latitude: number;
  longitude: number;
  place: string; // "City, CC", may be empty
  source: string; // the service that answered
  cached: boolean;
  nextLookup: string; // "HH:MM" when a new lookup is allowed
}

type InitCallback = (data: InitData) => void;
type MonitorsCallback = (monitors: MonitorData[]) => void;
type RemoteSessionCallback = (remote: boolean) => void;

let initCallback: InitCallback | null = null;
let monitorsCallback: MonitorsCallback | null = null;
let remoteSessionCallback: RemoteSessionCallback | null = null;
let updateResultCallback: ((result: UpdateResult) => void) | null = null;
let updateProgressCallback: ((progress: UpdateProgress) => void) | null = null;
let locationResultCallback: ((result: LocationResult) => void) | null = null;

export function onInit(cb: InitCallback) {
  initCallback = cb;
}

export function onMonitors(cb: MonitorsCallback) {
  monitorsCallback = cb;
  return () => {
    if (monitorsCallback === cb) monitorsCallback = null;
  };
}

// The session moved between the console and Remote Desktop while the
// dialog is open.
export function onRemoteSession(cb: RemoteSessionCallback) {
  remoteSessionCallback = cb;
  return () => {
    if (remoteSessionCallback === cb) remoteSessionCallback = null;
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

(window as unknown as Record<string, unknown>).onRemoteSession = (
  remote: boolean
) => {
  if (remoteSessionCallback) remoteSessionCallback(remote);
};

(window as unknown as Record<string, unknown>).onUpdateResult = (
  result: UpdateResult
) => {
  if (updateResultCallback) updateResultCallback(result);
};

(window as unknown as Record<string, unknown>).onUpdateProgress = (
  progress: UpdateProgress
) => {
  if (updateProgressCallback) updateProgressCallback(progress);
};

(window as unknown as Record<string, unknown>).onLocationResult = (
  result: LocationResult
) => {
  if (locationResultCallback) locationResultCallback(result);
};

export function onLocationResult(cb: (result: LocationResult) => void) {
  locationResultCallback = cb;
  return () => {
    if (locationResultCallback === cb) locationResultCallback = null;
  };
}

export function detectLocation() {
  post({ action: "detectLocation" });
}

export function cancelLocationDetection() {
  post({ action: "cancelLocation" });
}

export function onUpdateResult(cb: (result: UpdateResult) => void) {
  updateResultCallback = cb;
  return () => {
    if (updateResultCallback === cb) updateResultCallback = null;
  };
}

export function onUpdateProgress(cb: (progress: UpdateProgress) => void) {
  updateProgressCallback = cb;
  return () => {
    if (updateProgressCallback === cb) updateProgressCallback = null;
  };
}

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

export function hideMonitor(uid: number) {
  post({ action: "hideMonitor", uid });
}

export function setAllowBelowMinimum(enabled: boolean) {
  post({ action: "setAllowBelowMinimum", enabled });
}

export function refreshMonitors() {
  post({ action: "refreshMonitors" });
}

// Ends the pause for every scheduled monitor, not just the card it was clicked on.
export function resumeSchedule() {
  post({ action: "resumeSchedule" });
}

export function configReady(checkAutomatically = false) {
  post({ action: "configReady", checkAutomatically });
}

export function checkForUpdate(automatic = false) {
  post({ action: "checkUpdate", automatic });
}

export function cancelUpdateCheck() {
  post({ action: "cancelUpdateCheck" });
}

export function installUpdate(reopenSettings = false) {
  post({ action: "installUpdate", reopenSettings });
}

export function dismissUpdate() {
  post({ action: "dismissUpdate" });
}

export function ignoreUpdateVersion(version: string) {
  post({ action: "ignoreUpdateVersion", version });
}

export function dismissUpdateConfirmation() {
  post({ action: "dismissUpdateConfirmation" });
}

export function saveSettings(
  debugLog: boolean,
  autoCheckForUpdates: boolean,
  startWithWindows: boolean,
  pauseInRemoteSession: boolean,
  brightnessKeys: boolean,
  trayTarget: string,
  trayPresets: number[],
  schedule: ScheduleSettings,
  monitors: MonitorData[]
) {
  post({
    action: "saveSettings",
    debugLog,
    autoCheckForUpdates,
    startWithWindows,
    pauseInRemoteSession,
    brightnessKeys,
    trayTarget,
    trayPresets: trayPresets.join(","),
    scheduleEnabled: schedule.enabled,
    latitude: schedule.latitude,
    longitude: schedule.longitude,
    dayLevel: schedule.dayLevel,
    nightLevel: schedule.nightLevel,
    dawnStartOffset: schedule.dawnStartOffset,
    dawnEndOffset: schedule.dawnEndOffset,
    duskStartOffset: schedule.duskStartOffset,
    duskEndOffset: schedule.duskEndOffset,
    cycleResetMinutes: schedule.cycleResetMinutes,
    deepSleepEnabled: schedule.deepSleepEnabled,
    deepSleepLevel: schedule.deepSleepLevel,
    deepSleepMinutes: schedule.deepSleepMinutes,
    scheduledKeys: monitors.filter((m) => !m.hidden && schedule.scheduledKeys.includes(m.key)).map((m) => m.key).join(","),
    scheduleMonitorKeys: monitors.filter((m) => !m.hidden).map((m) => m.key).join(","),
  });
}

export function closeDialog() {
  post({ action: "close" });
}

// The size the page wants the host window to provide, in CSS pixels. The
// host converts it to physical pixels, adds the window frame, clamps the
// result to the monitor's work area, and re-centers the dialog. The width
// switches between the single- and two-column layouts.
export const SINGLE_COLUMN_WIDTH = 480;
export const TWO_COLUMN_WIDTH = 2 * SINGLE_COLUMN_WIDTH + 40;

let desiredContentWidth = SINGLE_COLUMN_WIDTH;
// The window starts at the single-column width; a width is only sent when
// the layout changes, so a user who widened the dialog by hand keeps it.
let lastSentWidth = SINGLE_COLUMN_WIDTH;

export function setDesiredContentWidth(width: number) {
  desiredContentWidth = width;
}

export function reportSize(height: number) {
  const message: { action: string; height: number; width?: number } = {
    action: "resize",
    height,
  };
  if (desiredContentWidth !== lastSentWidth) {
    message.width = desiredContentWidth;
    lastSentWidth = desiredContentWidth;
  }
  post(message);
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
