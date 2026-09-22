import { reconcileScheduleSelection } from "./lib/scheduleSelection";
import { useEffect, useLayoutEffect, useRef, useState } from "react";
import {
  type ConfigData,
  type MonitorData,
  type ScheduleSettings,
  type UpdateResult,
  SINGLE_COLUMN_WIDTH,
  TWO_COLUMN_WIDTH,
  TRAY_TARGET_NONE,
  TRAY_TARGET_ALL,
  parseTrayPresets,
  saveSettings,
  closeDialog,
  checkForUpdate,
  cancelUpdateCheck,
  configReady,
  installUpdate,
  dismissUpdate,
  ignoreUpdateVersion,
  dismissUpdateConfirmation,
  onUpdateResult,
  onUpdateProgress,
  setBrightness,
  setAllBrightness,
  setMonitorSoftwareOnly,
  setAllowBelowMinimum,
  hideMonitor,
  refreshMonitors,
  resumeSchedule,
  setDesiredContentWidth,
} from "./lib/bridge";
import { Button } from "./components/ui/button";
import { Checkbox } from "./components/ui/checkbox";
import { Input } from "./components/ui/input";
import { Label } from "./components/ui/label";
import { Select } from "./components/ui/select";
import { Separator } from "./components/ui/separator";
import { Slider } from "./components/ui/slider";
import ScheduleSection, { parseCoordinate } from "./ScheduleSection";

interface Props {
  config: ConfigData;
  monitors: MonitorData[];
  // Viewed through Remote Desktop: the monitors are shown as they were at
  // the console and everything that would touch them is disabled.
  remoteSession: boolean;
  updateCompletedVersion: string;
}

// Slider moves stream at display refresh rate; DDC/CI takes tens of
// milliseconds per write, so sends are throttled with the latest value
// always delivered last.
const SEND_INTERVAL_MS = 40;
const MASTER_KEY = -1;

function useThrottledSender() {
  const timers = useRef(new Map<number, number>());
  const pending = useRef(new Map<number, () => void>());

  useEffect(() => {
    const activeTimers = timers.current;
    const queued = pending.current;
    return () => {
      activeTimers.forEach((id) => window.clearTimeout(id));
      queued.forEach((send) => send());
    };
  }, []);

  return (key: number, send: () => void) => {
    if (timers.current.has(key)) {
      pending.current.set(key, send);
      return;
    }
    send();
    const flush = () => {
      const queued = pending.current.get(key);
      pending.current.delete(key);
      if (queued) {
        queued();
        timers.current.set(key, window.setTimeout(flush, SEND_INTERVAL_MS));
      } else {
        timers.current.delete(key);
      }
    };
    timers.current.set(key, window.setTimeout(flush, SEND_INTERVAL_MS));
  };
}

function clamp(value: number, min: number, max: number) {
  return Math.min(max, Math.max(min, value));
}

function formatValue(value: number) {
  return `${value}%`;
}

function ModeBadge({ monitor }: { monitor: MonitorData }) {
  let text = "Detecting…";
  let className = "border-neutral-200 bg-neutral-50 text-neutral-500";
  if (monitor.mode === "hardware") {
    text = monitor.builtin ? "Hardware (built-in)" : "Hardware (DDC/CI)";
    className = "border-emerald-200 bg-emerald-50 text-emerald-700";
  } else if (monitor.mode === "waiting") {
    text = monitor.builtin ? "Built-in not answering" : "DDC/CI not answering";
    className = "border-amber-200 bg-amber-50 text-amber-700";
  } else if (monitor.mode === "software") {
    text = monitor.forceSoftware
      ? "Software (chosen)"
      : monitor.hardware === "unavailable"
        ? "Software (no DDC/CI)"
        : "Software";
    className = "border-sky-200 bg-sky-50 text-sky-700";
  }
  return (
    <span
      className={`shrink-0 whitespace-nowrap rounded border px-1.5 py-0.5 text-[10px] font-medium leading-none ${className}`}
    >
      {text}
    </span>
  );
}

function ScheduleBadge({ monitor }: { monitor: MonitorData }) {
  if (monitor.pausedUntil) {
    return (
      <span
        className="shrink-0 whitespace-nowrap rounded border border-amber-200 bg-amber-50 px-1.5 py-0.5 text-[10px] font-medium leading-none text-amber-700"
        title={`Paused after a manual change; the schedule takes over again at ${monitor.pausedUntil}`}
      >
        Auto paused
      </span>
    );
  }
  return (
    <span
      className="shrink-0 whitespace-nowrap rounded border border-indigo-200 bg-indigo-50 px-1.5 py-0.5 text-[10px] font-medium leading-none text-indigo-700"
      title="Follows the sun-based schedule"
    >
      Auto
    </span>
  );
}

interface MonitorCardProps {
  monitor: MonitorData;
  value: number;
  canHide: boolean;
  locked: boolean;
  scheduleEnabled: boolean;
  onChange: (value: number) => void;
  onSoftwareOnlyChange: (softwareOnly: boolean) => void;
  onHide: () => void;
}

function MonitorCard({
  monitor,
  value,
  canHide,
  locked,
  scheduleEnabled,
  onChange,
  onSoftwareOnlyChange,
  onHide,
}: MonitorCardProps) {
  const sliderId = `brightness-${monitor.uid}`;
  const softwareId = `software-${monitor.uid}`;
  const details = [
    monitor.device.replace(/^\\\\\.\\/, ""),
    monitor.width && monitor.height
      ? `${monitor.width}×${monitor.height}`
      : "",
    monitor.primary ? "Primary" : "",
  ]
    .filter(Boolean)
    .join(" · ");

  return (
    <div className="space-y-1.5 rounded-md border border-neutral-200 px-3 py-2">
      <div className="flex items-center justify-between gap-2">
        <div className="min-w-0">
          <Label htmlFor={sliderId} className="block truncate text-xs">
            {monitor.name}
          </Label>
          <p className="truncate text-[11px] leading-snug text-neutral-500">
            {details}
          </p>
        </div>
        <div className="flex shrink-0 items-center gap-2">
          {scheduleEnabled && monitor.scheduled && <ScheduleBadge monitor={monitor} />}
          <ModeBadge monitor={monitor} />
          <span className="w-10 text-right text-xs tabular-nums">
            {formatValue(value)}
          </span>
          <Button
            variant="ghost"
            size="sm"
            className="h-6 px-1.5 text-[11px] text-neutral-500"
            disabled={!canHide || locked}
            title={
              locked
                ? "Not available during a Remote Desktop session"
                : canHide
                  ? "Restore this monitor's original brightness, then stop controlling it and remove it from the list until the next rescan"
                  : "The last monitor in the list cannot be hidden"
            }
            aria-label={`Hide ${monitor.name}`}
            onClick={onHide}
          >
            Hide
          </Button>
        </div>
      </div>
      <Slider
        id={sliderId}
        min={monitor.min}
        max={monitor.max}
        step={1}
        value={value}
        disabled={locked}
        onChange={(e) => onChange(Number(e.target.value))}
      />
      {(monitor.hardware === "available" || monitor.knownHardware) && (
        <div className="flex items-center gap-2">
          <Checkbox
            id={softwareId}
            checked={monitor.forceSoftware}
            disabled={locked}
            onChange={(e) => onSoftwareOnlyChange(e.target.checked)}
          />
          <Label
            htmlFor={softwareId}
            className="cursor-pointer text-[11px] font-normal text-neutral-600"
          >
            Software dimming only (leave the monitor's own brightness alone)
          </Label>
        </div>
      )}
      {monitor.mode === "software" && monitor.hardware === "unavailable" && !monitor.forceSoftware && (
        <p className="text-[11px] leading-snug text-neutral-500">
          This monitor did not answer DDC/CI, so it is dimmed in software. If
          it has a DDC/CI option in its on-screen menu, enable it and rescan.
        </p>
      )}
      {monitor.mode === "waiting" && (
        <p className="text-[11px] leading-snug text-neutral-500">
          {monitor.builtin
            ? "Windows' brightness control for this built-in display is not responding now."
            : "This monitor answered DDC/CI before but is not responding now."}{" "}
          Its backlight is left exactly as it is and it is retried
          automatically; the slider applies once it answers. To dim it in
          software in the meantime, tick Software dimming only.
        </p>
      )}
      {scheduleEnabled && monitor.scheduled && monitor.pausedUntil && (
        <p className="text-[11px] leading-snug text-neutral-500">
          The schedule is paused after a manual change and resumes at{" "}
          {monitor.pausedUntil} for every monitor it controls.{" "}
          <button
            type="button"
            className="underline hover:text-neutral-900"
            onClick={() => resumeSchedule()}
          >
            Resume now
          </button>
        </p>
      )}
      {monitor.error && (
        <p className="text-[11px] leading-snug text-red-600">{monitor.error}</p>
      )}
    </div>
  );
}

function initialSchedule(config: ConfigData, monitors: MonitorData[]): ScheduleSettings {
  const s = config.schedule;
  return {
    enabled: s?.enabled ?? false,
    latitude: s?.hasLocation ? String(s.latitude) : "",
    longitude: s?.hasLocation ? String(s.longitude) : "",
    dayLevel: s?.dayLevel ?? 100,
    nightLevel: s?.nightLevel ?? 30,
    dawnStartOffset: s?.dawnStartOffset ?? -30,
    dawnEndOffset: s?.dawnEndOffset ?? 30,
    duskStartOffset: s?.duskStartOffset ?? -30,
    duskEndOffset: s?.duskEndOffset ?? 30,
    cycleResetMinutes: s?.cycleResetMinutes ?? 240,
    // Hidden monitors are not listed, so only visible ones count here;
    // a hidden one keeps its own flag until the next rescan.
    scheduledKeys: monitors.filter((m) => m.scheduled && !m.hidden).map((m) => m.key),
  };
}

export default function ConfigView({
  config,
  monitors,
  remoteSession,
  updateCompletedVersion,
}: Props) {
  const [values, setValues] = useState<Record<number, number>>({});
  const [allowBelowMinimum, setAllowBelowMinimumState] = useState(
    config.allowBelowMinimum ?? false
  );
  const [debugLog, setDebugLog] = useState(config.debugLog ?? false);
  const [autoCheckForUpdates, setAutoCheckForUpdates] = useState(
    config.autoCheckForUpdates ?? true
  );
  const [pauseInRemoteSession, setPauseInRemoteSession] = useState(
    config.pauseInRemoteSession ?? true
  );
  const [brightnessKeys, setBrightnessKeys] = useState(config.brightnessKeys ?? false);
  const [trayTarget, setTrayTarget] = useState(config.trayTarget ?? TRAY_TARGET_NONE);
  // Shown as "100, 75, 50"; only what parses is saved.
  const [trayPresets, setTrayPresets] = useState(
    (config.trayPresets ?? "").split(",").filter(Boolean).join(", ")
  );
  const trayPresetsRef = useRef<HTMLInputElement>(null);
  const [updateChecking, setUpdateChecking] = useState(
    config.updateCheckPending ?? false
  );
  const [updateCancelling, setUpdateCancelling] = useState(false);
  const [reopenSettings, setReopenSettings] = useState(false);
  const [updateSpeedKbps, setUpdateSpeedKbps] = useState<number | null>(null);
  const [updateAlert, setUpdateAlert] = useState<UpdateResult | null>(() =>
    updateCompletedVersion
      ? {
          status: "completed",
          title: "Update complete",
          message: `Not Too Bright has been updated to version ${updateCompletedVersion}.`,
          currentVersion: "",
          remoteVersion: "",
          automatic: false,
        }
      : null
  );
  const automaticUpdateStarted = useRef(false);

  useEffect(() => {
    const removeResultListener = onUpdateResult((result) => {
      setReopenSettings(false);
      setUpdateChecking(false);
      setUpdateCancelling(false);
      setUpdateSpeedKbps(null);
      if (result.status === "cancelled") {
        setUpdateAlert((current) =>
          result.automatic && current?.status === "completed" ? current : null
        );
      } else if (result.automatic && result.status !== "newer") {
        setUpdateAlert((current) =>
          current?.status === "completed" ? current : null
        );
      } else {
        setUpdateAlert(result);
      }
    });
    const removeProgressListener = onUpdateProgress((progress) => {
      setUpdateSpeedKbps(Math.max(0, Math.round(progress.kilobytesPerSecond)));
    });

    const shouldCheckAutomatically =
      config.autoCheckForUpdates &&
      !updateCompletedVersion &&
      !config.updateCheckPending &&
      !config.updatePromptPending &&
      !automaticUpdateStarted.current;
    if (shouldCheckAutomatically) {
      automaticUpdateStarted.current = true;
      setUpdateChecking(true);
    }
    configReady(shouldCheckAutomatically);

    return () => {
      removeResultListener();
      removeProgressListener();
    };
  }, [
    config.autoCheckForUpdates,
    config.updateCheckPending,
    config.updatePromptPending,
    updateCompletedVersion,
  ]);

  function handleUpdate() {
    if (updateChecking) {
      setUpdateCancelling(true);
      cancelUpdateCheck();
      return;
    }
    setUpdateAlert(null);
    setUpdateChecking(true);
    setUpdateCancelling(false);
    setUpdateSpeedKbps(null);
    checkForUpdate(false);
  }

  function handleInstallUpdate() {
    setUpdateChecking(true);
    setUpdateCancelling(false);
    setUpdateSpeedKbps(null);
    installUpdate(reopenSettings);
    setReopenSettings(false);
  }

  function handleDismissUpdate() {
    setReopenSettings(false);
    if (updateAlert?.status === "completed") {
      dismissUpdateConfirmation();
    } else {
      dismissUpdate();
    }
    setUpdateAlert(null);
  }

  function handleIgnoreUpdateVersion() {
    if (!updateAlert?.remoteVersion) return;
    setReopenSettings(false);
    ignoreUpdateVersion(updateAlert.remoteVersion);
    setUpdateAlert(null);
  }
  const [schedule, setSchedule] = useState<ScheduleSettings>(() => initialSchedule(config, monitors));
  const seenScheduleKeys = useRef(new Set(monitors.filter((m) => !m.hidden).map((m) => m.key)));
  useEffect(() => {
    const seen = seenScheduleKeys.current;
    const visible = monitors.filter((m) => !m.hidden);
    if (!visible.some((m) => !seen.has(m.key))) return;
    // Capture the previous set: state updaters may run after this effect.
    const previous = new Set(seen);
    setSchedule((current) => ({
      ...current,
      scheduledKeys: reconcileScheduleSelection(current.scheduledKeys, previous, visible),
    }));
    for (const monitor of visible) seen.add(monitor.key);
  }, [monitors]);
  const [scheduleError, setScheduleError] = useState("");
  const throttledSend = useThrottledSender();
  // The schedule section is tall; on a wide enough screen the dialog shows
  // it beside the monitors instead of below them.
  const twoColumn =
    schedule.enabled && window.screen.availWidth >= TWO_COLUMN_WIDTH + 96;
  useLayoutEffect(() => {
    setDesiredContentWidth(twoColumn ? TWO_COLUMN_WIDTH : SINGLE_COLUMN_WIDTH);
  }, [twoColumn]);
  const visibleMonitors = monitors.filter((m) => !m.hidden);
  const hiddenCount = monitors.length - visibleMonitors.length;
  const canHide = visibleMonitors.length > 1;

  // The host is the source of truth: whenever it pushes a monitor list
  // (probe finished, display change, mode switch) adopt its values.
  useEffect(() => {
    setValues(() => {
      const next: Record<number, number> = {};
      for (const m of monitors) next[m.uid] = m.value;
      return next;
    });
  }, [monitors]);

  function valueOf(monitor: MonitorData) {
    const v = values[monitor.uid] ?? monitor.value;
    return clamp(v, monitor.min, monitor.max);
  }

  function handleMonitorChange(monitor: MonitorData, value: number) {
    setValues((current) => ({ ...current, [monitor.uid]: value }));
    throttledSend(monitor.uid, () => setBrightness(monitor.uid, value));
  }

  const masterMin = visibleMonitors.reduce((min, m) => Math.max(min, m.min), 0);
  const masterValue =
    visibleMonitors.length > 0
      ? Math.round(
          visibleMonitors.reduce((sum, m) => sum + valueOf(m), 0) /
            visibleMonitors.length
        )
      : 100;

  function handleMasterChange(value: number) {
    setValues((current) => {
      const next = { ...current };
      for (const m of visibleMonitors) next[m.uid] = clamp(value, m.min, m.max);
      return next;
    });
    throttledSend(MASTER_KEY, () => setAllBrightness(value));
  }

  function handleAllowBelowMinimum(enabled: boolean) {
    setAllowBelowMinimumState(enabled);
    setAllowBelowMinimum(enabled);
  }

  const minLevel = allowBelowMinimum ? -90 : 0;
  // The preset field only matters once the tray menu has a target; its
  // error is shown live under the field and blocks Save while visible.
  const showTrayPresets = trayTarget !== TRAY_TARGET_NONE;
  const trayPresetsParsed = parseTrayPresets(trayPresets, minLevel);

  function handleSave() {
    if (schedule.enabled) {
      const lat = parseCoordinate(schedule.latitude, 90);
      const lon = parseCoordinate(schedule.longitude, 180);
      if (lat === null || lon === null) {
        setScheduleError(
          "Enter a latitude between -90 and 90 and a longitude between -180 and 180."
        );
        return;
      }
    }
    setScheduleError("");
    if (showTrayPresets && trayPresetsParsed.error) {
      trayPresetsRef.current?.focus();
      return;
    }
    saveSettings(
      debugLog,
      autoCheckForUpdates,
      pauseInRemoteSession,
      brightnessKeys,
      trayTarget,
      trayPresetsParsed.values,
      schedule,
      monitors
    );
  }
  const scheduleSection = (
    <ScheduleSection
      settings={schedule}
      onChange={(next) => {
        setSchedule(next);
        if (scheduleError) setScheduleError("");
      }}
      monitors={visibleMonitors}
      minLevel={minLevel}
      error={scheduleError}
    />
  );

  return (
    <div
      className="p-4 space-y-3"
      style={twoColumn ? { width: TWO_COLUMN_WIDTH, maxWidth: "100%" } : undefined}
    >
      <div className={twoColumn ? "grid grid-cols-2 items-start gap-x-6" : "space-y-3"}>
      <div className="space-y-3">
      <div className="flex items-start justify-between gap-3">
        <div className="space-y-0.5">
          <Label>Brightness</Label>
          <p className="text-neutral-500 text-[11px] leading-snug">
            Changes apply immediately and are remembered per monitor.
          </p>
        </div>
        <Button
          variant="outline"
          size="sm"
          className="shrink-0"
          disabled={remoteSession}
          title={
            remoteSession
              ? "Not available during a Remote Desktop session"
              : "Detect monitors again and show any hidden ones"
          }
          onClick={refreshMonitors}
        >
          Rescan
        </Button>
      </div>

      {remoteSession && (
        <p className="rounded-md border border-amber-200 bg-amber-50 px-3 py-2 text-[11px] leading-snug text-amber-800">
          Paused: This session is being viewed through Remote Desktop. The
          monitors are listed as they were at the computer and keep their
          brightness; they can be adjusted again once you are back at it.
        </p>
      )}

      {visibleMonitors.length === 0 && (
        <p className="rounded-md border border-neutral-200 px-3 py-2 text-[11px] leading-snug text-neutral-500">
          {remoteSession
            ? "No monitors are known yet. They are detected once you are back at the computer."
            : monitors.length === 0
              ? "No monitors were detected. Connect a display and choose Rescan."
              : "Every connected monitor is hidden. Choose Rescan to show them again."}
        </p>
      )}

      {visibleMonitors.length > 1 && (
        <div className="space-y-1.5 rounded-md border border-neutral-200 bg-neutral-50 px-3 py-2">
          <div className="flex items-center justify-between gap-2">
            <Label htmlFor="brightness-all" className="text-xs">
              All monitors
            </Label>
            <span className="w-10 text-right text-xs tabular-nums">
              {formatValue(clamp(masterValue, masterMin, 100))}
            </span>
          </div>
          <Slider
            id="brightness-all"
            min={masterMin}
            max={100}
            step={1}
            value={clamp(masterValue, masterMin, 100)}
            disabled={remoteSession}
            onChange={(e) => handleMasterChange(Number(e.target.value))}
          />
        </div>
      )}

      {visibleMonitors.map((monitor) => (
        <MonitorCard
          key={monitor.uid}
          monitor={monitor}
          value={valueOf(monitor)}
          canHide={canHide}
          locked={remoteSession}
          scheduleEnabled={config.schedule?.enabled ?? false}
          onChange={(value) => handleMonitorChange(monitor, value)}
          onSoftwareOnlyChange={(softwareOnly) =>
            setMonitorSoftwareOnly(monitor.uid, softwareOnly)
          }
          onHide={() => hideMonitor(monitor.uid)}
        />
      ))}

      {hiddenCount > 0 && (
        <p className="text-[11px] leading-snug text-neutral-500">
          {hiddenCount === 1
            ? "1 monitor is hidden and not controlled."
            : `${hiddenCount} monitors are hidden and not controlled.`}{" "}
          Rescan shows hidden monitors again.
        </p>
      )}

      <div className="flex items-start gap-2 pt-1">
        <Checkbox
          id="allowBelowMinimum"
          className="mt-0.5"
          checked={allowBelowMinimum}
          onChange={(e) => handleAllowBelowMinimum(e.target.checked)}
        />
        <div className="space-y-0.5">
          <Label htmlFor="allowBelowMinimum" className="cursor-pointer">
            Allow dimming below the hardware minimum
          </Label>
          <p className="text-neutral-500 text-[11px] leading-snug">
            Extends the sliders of hardware-controlled monitors below 0%.
            Negative values keep the backlight at its minimum and add software
            dimming on top, down to -90%. Applies immediately.
          </p>
        </div>
      </div>
      </div>

      {twoColumn ? (
        <div className="space-y-3">{scheduleSection}</div>
      ) : (
        <>
          <Separator />
          {scheduleSection}
        </>
      )}
      </div>

      <Separator />

      <div className="space-y-1 pt-1">
        <Label htmlFor="trayTarget">Tray menu brightness control</Label>
        <Select
          id="trayTarget"
          value={trayTarget}
          onChange={(e) => setTrayTarget(e.target.value)}
        >
          <option value={TRAY_TARGET_NONE}>None</option>
          <option value={TRAY_TARGET_ALL}>All monitors</option>
          {visibleMonitors.map((m) => (
            <option key={m.uid} value={m.key}>
              {m.name}
            </option>
          ))}
          {trayTarget !== TRAY_TARGET_NONE &&
            trayTarget !== TRAY_TARGET_ALL &&
            !visibleMonitors.some((m) => m.key === trayTarget) && (
              <option value={trayTarget}>Selected monitor (not available right now)</option>
            )}
        </Select>
        {showTrayPresets && (
          <div className="space-y-1 pt-1">
            <Label htmlFor="trayPresets">Preset levels</Label>
            <Input
              id="trayPresets"
              ref={trayPresetsRef}
              placeholder="e.g. 100, 75, 50, 25"
              value={trayPresets}
              onChange={(e) =>
                // Only digits, commas and spaces can be typed (a minus sign
                // too when the extended range is on); the rest is dropped.
                setTrayPresets(
                  e.target.value.replace(minLevel < 0 ? /[^0-9,\s-]/g : /[^0-9,\s]/g, "")
                )
              }
              aria-invalid={trayPresetsParsed.error !== null}
              className={trayPresetsParsed.error ? "border-red-500" : ""}
            />
            {trayPresetsParsed.error && (
              <p className="text-red-600 text-[11px]">{trayPresetsParsed.error}</p>
            )}
          </div>
        )}
        <p className="text-neutral-500 text-[11px] leading-snug">
          {showTrayPresets
            ? `The tray menu gets Increase and Decrease brightness (10% steps) plus ` +
              `the preset levels above: whole numbers from ${minLevel} to 100, ` +
              `separated by commas, listed in that order. Manual changes pause ` +
              `scheduled monitors.`
            : "Choose All monitors or one monitor to add Increase brightness, " +
              "Decrease brightness and preset levels to the tray menu."}
        </p>
      </div>

      <div className="flex items-start gap-2 pt-1">
        <Checkbox
          id="brightnessKeys"
          className="mt-0.5"
          checked={brightnessKeys}
          onChange={(e) => setBrightnessKeys(e.target.checked)}
        />
        <div className="space-y-0.5">
          <Label htmlFor="brightnessKeys" className="cursor-pointer">
            Use the keyboard's brightness keys
          </Label>
          <p className="text-neutral-500 text-[11px] leading-snug">
            The Brightness Up and Brightness Down keys change every external
            monitor by 10% per press, whatever window is focused, and count as
            a manual change for the schedule. Windows keeps showing its own
            brightness indicator and moves a built-in display itself.
          </p>
        </div>
      </div>

      <div className="flex items-start gap-2 pt-1">
        <Checkbox
          id="pauseInRemoteSession"
          className="mt-0.5"
          checked={pauseInRemoteSession}
          onChange={(e) => setPauseInRemoteSession(e.target.checked)}
        />
        <div className="space-y-0.5">
          <Label htmlFor="pauseInRemoteSession" className="cursor-pointer">
            Pause while connected through Remote Desktop
          </Label>
          <p className="text-neutral-500 text-[11px] leading-snug">
            Leaves the monitors exactly as they are while this session is
            viewed remotely and picks up again at the computer. Turn off only
            if this Windows session is always used through Remote Desktop.
          </p>
        </div>
      </div>

      <div className="flex items-start gap-2 pt-1">
        <Checkbox
          id="autoCheckForUpdates"
          className="mt-0.5"
          checked={autoCheckForUpdates}
          onChange={(e) => setAutoCheckForUpdates(e.target.checked)}
        />
        <div className="space-y-0.5">
          <Label htmlFor="autoCheckForUpdates" className="cursor-pointer">
            Automatically check for updates
          </Label>
          <p className="text-neutral-500 text-[11px] leading-snug">
            Checks at startup, whenever this dialog opens, and every 60 minutes.
            Prompts only when a newer version is available.
          </p>
        </div>
      </div>

      <div className="flex items-start gap-2 pt-1">
        <Checkbox
          id="debugLog"
          className="mt-0.5"
          checked={debugLog}
          onChange={(e) => setDebugLog(e.target.checked)}
        />
        <div className="space-y-0.5">
          <Label htmlFor="debugLog" className="cursor-pointer">
            Enable debug logging
          </Label>
          <p className="text-neutral-500 text-[11px] leading-snug">
            Writes monitor detection, DDC/CI and built-in display results to{" "}
            %LOCALAPPDATA%\NotTooBright\debug.log. Useful when reporting
            issues; leave off for normal use.
          </p>
        </div>
      </div>

      <div className="flex items-center justify-between gap-3 pt-1">
        <span
          className="select-none whitespace-nowrap text-[11px] leading-none tabular-nums text-neutral-400"
          title="Application version"
        >
          v{__APP_VERSION__}
        </span>
        <div className="flex items-center gap-2">
          <Button
            variant={updateChecking ? "destructive" : "outline"}
            size="sm"
            className="min-w-[5rem]"
            disabled={updateCancelling}
            aria-label={
              updateChecking ? "Stop update check and download" : undefined
            }
            title={
              updateChecking ? "Stop update check and download" : undefined
            }
            onClick={handleUpdate}
          >
            {updateCancelling
              ? "Stopping..."
              : updateChecking
                ? updateSpeedKbps === null
                  ? "Checking..."
                  : `Checking (${updateSpeedKbps}kb/s)...`
                : "Update"}
          </Button>
          <Button
            variant="outline"
            size="sm"
            className="min-w-[5rem]"
            onClick={closeDialog}
          >
            Cancel
          </Button>
          <Button size="sm" className="min-w-[5rem]" onClick={handleSave}>
            Save
          </Button>
        </div>
      </div>

      {updateAlert && (
        <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/35 p-4">
          <div
            role="alertdialog"
            aria-modal="true"
            aria-labelledby="update-alert-title"
            aria-describedby="update-alert-message"
            className="w-full max-w-sm space-y-3 rounded-lg border border-neutral-200 bg-white p-4 shadow-xl"
          >
            <div className="space-y-1">
              <h2 id="update-alert-title" className="text-sm font-semibold">
                {updateAlert.title}
              </h2>
              <p
                id="update-alert-message"
                className="text-xs leading-relaxed text-neutral-600"
              >
                {updateAlert.message}
              </p>
            </div>
            {updateAlert.currentVersion && updateAlert.remoteVersion && (
              <dl className="grid grid-cols-[1fr_auto] gap-x-4 gap-y-1 rounded-md border border-neutral-200 bg-neutral-50 px-3 py-2 text-xs">
                <dt className="text-neutral-500">Current version</dt>
                <dd className="font-medium tabular-nums text-neutral-900">
                  {updateAlert.currentVersion}
                </dd>
                <dt className="text-neutral-500">Remote version</dt>
                <dd className="font-medium tabular-nums text-neutral-900">
                  {updateAlert.remoteVersion}
                </dd>
              </dl>
            )}
            {(updateAlert.status === "newer" ||
              updateAlert.status === "same") && (
              <div className="flex items-center gap-2">
                <Checkbox
                  id="reopenSettings"
                  checked={reopenSettings}
                  disabled={updateChecking}
                  onChange={(e) => setReopenSettings(e.target.checked)}
                />
                <Label htmlFor="reopenSettings" className="cursor-pointer">
                  Reopen settings after update
                </Label>
              </div>
            )}
            <div className="flex justify-end gap-2">
              {updateAlert.status === "newer" && updateAlert.automatic && (
                <Button
                  variant="outline"
                  size="sm"
                  disabled={updateChecking}
                  onClick={handleIgnoreUpdateVersion}
                >
                  Ignore this version
                </Button>
              )}
              {(updateAlert.status === "newer" ||
                updateAlert.status === "same") && (
                <Button
                  variant="outline"
                  size="sm"
                  autoFocus
                  disabled={updateChecking}
                  onClick={handleDismissUpdate}
                >
                  Cancel
                </Button>
              )}
              <Button
                size="sm"
                autoFocus={
                  updateAlert.status !== "newer" &&
                  updateAlert.status !== "same"
                }
                disabled={updateChecking}
                onClick={
                  updateAlert.status === "newer" ||
                  updateAlert.status === "same"
                    ? handleInstallUpdate
                    : handleDismissUpdate
                }
              >
                {updateChecking
                  ? "Starting..."
                  : updateAlert.status === "same"
                    ? "Force update"
                    : updateAlert.status === "newer"
                      ? "Update"
                      : "OK"}
              </Button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
