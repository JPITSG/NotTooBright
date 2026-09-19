import type { MonitorData } from "./bridge";

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
