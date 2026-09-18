import { useEffect, useRef, useState } from "react";
import {
  type ConfigData,
  type MonitorData,
  saveSettings,
  closeDialog,
  setBrightness,
  setAllBrightness,
  setMonitorSoftwareOnly,
  setAllowBelowMinimum,
  refreshMonitors,
} from "./lib/bridge";
import { Button } from "./components/ui/button";
import { Checkbox } from "./components/ui/checkbox";
import { Label } from "./components/ui/label";
import { Separator } from "./components/ui/separator";
import { Slider } from "./components/ui/slider";

interface Props {
  config: ConfigData;
  monitors: MonitorData[];
  webView2Version: string;
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
    text = "Hardware (DDC/CI)";
    className = "border-emerald-200 bg-emerald-50 text-emerald-700";
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

interface MonitorCardProps {
  monitor: MonitorData;
  value: number;
  onChange: (value: number) => void;
  onSoftwareOnlyChange: (softwareOnly: boolean) => void;
}

function MonitorCard({
  monitor,
  value,
  onChange,
  onSoftwareOnlyChange,
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
          <ModeBadge monitor={monitor} />
          <span className="w-10 text-right text-xs tabular-nums">
            {formatValue(value)}
          </span>
        </div>
      </div>
      <Slider
        id={sliderId}
        min={monitor.min}
        max={monitor.max}
        step={1}
        value={value}
        onChange={(e) => onChange(Number(e.target.value))}
      />
      {monitor.hardware === "available" && (
        <div className="flex items-center gap-2">
          <Checkbox
            id={softwareId}
            checked={monitor.forceSoftware}
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
      {monitor.hardware === "unavailable" && !monitor.forceSoftware && (
        <p className="text-[11px] leading-snug text-neutral-500">
          This monitor did not answer DDC/CI, so it is dimmed in software. If
          it has a DDC/CI option in its on-screen menu, enable it and rescan.
        </p>
      )}
      {monitor.error && (
        <p className="text-[11px] leading-snug text-red-600">{monitor.error}</p>
      )}
    </div>
  );
}

export default function ConfigView({
  config,
  monitors,
  webView2Version,
}: Props) {
  const [values, setValues] = useState<Record<number, number>>({});
  const [allowBelowMinimum, setAllowBelowMinimumState] = useState(
    config.allowBelowMinimum ?? false
  );
  const [startWithWindows, setStartWithWindows] = useState(
    config.startWithWindows ?? false
  );
  const [debugLog, setDebugLog] = useState(config.debugLog ?? false);
  const throttledSend = useThrottledSender();

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

  const masterMin = monitors.reduce((min, m) => Math.max(min, m.min), 0);
  const masterValue =
    monitors.length > 0
      ? Math.round(
          monitors.reduce((sum, m) => sum + valueOf(m), 0) / monitors.length
        )
      : 100;

  function handleMasterChange(value: number) {
    setValues(() => {
      const next: Record<number, number> = {};
      for (const m of monitors) next[m.uid] = clamp(value, m.min, m.max);
      return next;
    });
    throttledSend(MASTER_KEY, () => setAllBrightness(value));
  }

  function handleAllowBelowMinimum(enabled: boolean) {
    setAllowBelowMinimumState(enabled);
    setAllowBelowMinimum(enabled);
  }

  function handleSave() {
    saveSettings({ startWithWindows, debugLog });
  }

  return (
    <div className="p-4 space-y-3">
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
          onClick={refreshMonitors}
        >
          Rescan
        </Button>
      </div>

      {monitors.length === 0 && (
        <p className="rounded-md border border-neutral-200 px-3 py-2 text-[11px] leading-snug text-neutral-500">
          No monitors were detected. Connect a display and choose Rescan.
        </p>
      )}

      {monitors.length > 1 && (
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
            onChange={(e) => handleMasterChange(Number(e.target.value))}
          />
        </div>
      )}

      {monitors.map((monitor) => (
        <MonitorCard
          key={monitor.uid}
          monitor={monitor}
          value={valueOf(monitor)}
          onChange={(value) => handleMonitorChange(monitor, value)}
          onSoftwareOnlyChange={(softwareOnly) =>
            setMonitorSoftwareOnly(monitor.uid, softwareOnly)
          }
        />
      ))}

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

      <Separator />

      <div className="flex items-start gap-2 pt-1">
        <Checkbox
          id="startWithWindows"
          className="mt-0.5"
          checked={startWithWindows}
          onChange={(e) => setStartWithWindows(e.target.checked)}
        />
        <div className="space-y-0.5">
          <Label htmlFor="startWithWindows" className="cursor-pointer">
            Start NotTooBright when you sign in
          </Label>
          <p className="text-neutral-500 text-[11px] leading-snug">
            Adds the application to your account's startup programs so your
            brightness settings are restored after every sign-in. Only affects
            the current Windows user.
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
            Writes monitor detection and DDC/CI results to{" "}
            %LOCALAPPDATA%\NotTooBright\debug.log. Useful when reporting
            issues; leave off for normal use.
          </p>
        </div>
      </div>

      <div className="flex items-center justify-between gap-3 pt-1">
        <span
          className="select-none whitespace-nowrap text-[11px] leading-none tabular-nums text-neutral-400"
          title="Application version / WebView2 version"
        >
          v{__APP_VERSION__} / {webView2Version}
        </span>
        <div className="flex items-center gap-2">
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
    </div>
  );
}
