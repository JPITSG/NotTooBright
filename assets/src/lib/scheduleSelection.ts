import type { MonitorData, ScheduleSettings } from "./bridge";

// Remember explicit choices across temporary disconnects. Only monitors not
// previously shown in this dialog inherit their saved scheduling flag.
export function reconcileScheduleSelection(
  selected: string[],
  seen: ReadonlySet<string>,
  monitors: MonitorData[]
) {
  const next = new Set(selected);
  for (const monitor of monitors) {
    if (!monitor.hidden && !seen.has(monitor.key) && monitor.scheduled) {
      next.add(monitor.key);
    }
  }
  return Array.from(next);
}

// Save only updates the monitors currently shown. Disconnects, hidden
// monitors and enumeration order must not look like unsaved user edits.
export function hasScheduleChanges(
  current: ScheduleSettings,
  saved: ScheduleSettings,
  monitors: MonitorData[]
) {
  const { scheduledKeys: currentKeys, ...currentSettings } = current;
  const { scheduledKeys: savedKeys, ...savedSettings } = saved;
  return Object.entries(savedSettings).some(
    ([key, value]) => currentSettings[key as keyof typeof currentSettings] !== value
  ) || monitors.some((monitor) => !monitor.hidden &&
    currentKeys.includes(monitor.key) !== savedKeys.includes(monitor.key));
}
